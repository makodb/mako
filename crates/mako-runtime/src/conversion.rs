//! Offline engine conversion. The source log and data leases exclude a running
//! server. No listeners, elections or transaction decisions run here. Operators
//! must stop the whole replica group and convert every replica before restarting.
use crate::{
    application::{Index, Position},
    checkpoint,
    durable::DurableRows,
    log::{Command, JournalCommand},
    migration::Schema,
    node::{Config, EngineConfig},
    redo::Row,
    wire,
};
use parking_lot::Mutex;
use raft_host::{offline, StateMachine};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::Arc,
};
use sto_rocksdb::{
    distributed::{Prefix, Row as NativeRow},
    Database,
};

type Result<T> = std::result::Result<T, String>;
fn error(e: impl std::fmt::Display) -> String {
    e.to_string()
}
fn native(p: Position) -> sto_rocksdb::Position {
    sto_rocksdb::Position {
        index: p.index,
        term: p.term,
    }
}
fn position(p: sto_rocksdb::Position) -> Position {
    Position {
        index: p.index,
        term: p.term,
    }
}
fn sync(path: &Path) -> Result<()> {
    File::open(path).and_then(|f| f.sync_all()).map_err(error)
}

#[derive(Default)]
struct Image {
    index: Index,
    rows: BTreeMap<(u16, Vec<u8>), Row>,
}
enum Backing {
    Legacy(Arc<DurableRows>),
    Rocks(Arc<Database>),
    Logical(Mutex<Image>),
}
struct Machine {
    backing: Backing,
    owner: u32,
    durable: bool,
    identity: Vec<u8>,
    schema: Arc<Schema>,
}
impl Machine {
    fn checkpoint_identity(&self) -> checkpoint::Identity<'_> {
        checkpoint::Identity {
            owner: self.owner,
            engine: "Masstree",
            schema: &self.identity,
            durable: self.durable,
        }
    }
    fn index(&self) -> Result<Index> {
        match &self.backing {
            Backing::Legacy(d) => d.export_index(self.owner).map_err(error),
            Backing::Logical(i) => Ok(i.lock().index.clone()),
            Backing::Rocks(d) => {
                let p = position(d.applied().map_err(error)?);
                let index = Index::from_metadata(
                    d.distributed_metadata().map_err(error)?,
                    p.into(),
                    self.owner,
                    self.durable,
                )
                .map_err(error)?;
                let history = d.entry_history().map_err(error)?;
                if history.iter().any(|h| {
                    index.fingerprints.get(&h.position.index)
                        != Some(&(h.position.term, h.fingerprint))
                }) {
                    return Err("native and protocol replay identities disagree".into());
                }
                Ok(index)
            }
        }
    }
    fn check_row(&self, row: &Row) -> Result<()> {
        self.schema.key_for_log(row.table).map_err(error)?;
        let mut checked = row.clone();
        if checked.time_term == 0 {
            checked.time_term = 10;
        }
        checked.validate().map_err(error)
    }
    fn visit(&self, mut visitor: impl FnMut(Row) -> Result<()>) -> Result<()> {
        match &self.backing {
            Backing::Legacy(d) => d
                .visit(|row| visitor(row).map_err(|e| wire::Error::new(wire::ErrorCode::Fatal, e)))
                .map_err(error),
            Backing::Logical(i) => {
                for row in i.lock().rows.values() {
                    visitor(row.clone())?;
                }
                Ok(())
            }
            Backing::Rocks(d) => {
                let mut tables = d.list_tables().map_err(error)?;
                tables.sort_by_key(|t| t.id);
                for table in tables {
                    let id = u16::try_from(table.id).map_err(error)?;
                    self.schema.key_for_log(id).map_err(error)?;
                    let mut after = None;
                    loop {
                        let (page, end) = d
                            .distributed_page(table.id, after.as_deref(), 256)
                            .map_err(error)?;
                        for row in page {
                            after = Some(row.key.clone());
                            visitor(Row {
                                table: id,
                                key: row.key,
                                value: row.value,
                                time_term: row.time_term,
                            })?;
                        }
                        if end {
                            break;
                        }
                    }
                }
                Ok(())
            }
        }
    }
    fn load_logical(&self, path: &Path, p: Position) -> Result<Image> {
        let index = checkpoint::validate(path, self.checkpoint_identity(), p).map_err(error)?;
        let mut rows = BTreeMap::new();
        checkpoint::visit(path, |row| {
            self.check_row(&row)
                .map_err(|e| wire::Error::new(wire::ErrorCode::Fatal, e))?;
            if rows.insert((row.table, row.key.clone()), row).is_some() {
                return Err(wire::Error::new(
                    wire::ErrorCode::Fatal,
                    "duplicate checkpoint row",
                ));
            }
            Ok(())
        })
        .map_err(error)?;
        Ok(Image { index, rows })
    }
}
impl StateMachine for Machine {
    fn applied_position(&self) -> Result<raft_host::Position> {
        match &self.backing {
            Backing::Legacy(d) => d.position().map(|p| position(p).into()).map_err(error),
            Backing::Rocks(d) => d.applied().map(|p| position(p).into()).map_err(error),
            Backing::Logical(i) => Ok(i.lock().index.position.into()),
        }
    }
    fn apply(&self, p: raft_host::Position, bytes: Option<&[u8]>) -> Result<()> {
        let p: Position = p.into();
        let mut index = if matches!(self.backing, Backing::Legacy(_)) {
            Index::default()
        } else {
            self.index()?
        };
        let change = match bytes {
            None => {
                if let Backing::Legacy(d) = &self.backing {
                    return d.advance(native(p)).map_err(error);
                }
                index.no_op(p).map_err(error)?
            }
            Some(bytes) if bytes.get(4..8) == Some(2u32.to_le_bytes().as_slice()) => {
                let c: JournalCommand = wire::decode(bytes).map_err(error)?;
                if c.version != 2
                    || c.incarnation == 0
                    || c.sequence == 0
                    || c.record.transaction().durable != self.durable
                {
                    return Err("invalid offline transaction envelope".into());
                }
                if let mako_protocol::journal::Record::Prepare { rows, .. } = &c.record {
                    for row in rows {
                        self.check_row(row)?;
                    }
                }
                if let Backing::Legacy(d) = &self.backing {
                    return d
                        .apply_journal(native(p), bytes, self.owner, &c.record)
                        .map(|_| ())
                        .map_err(error);
                }
                index
                    .journal(p, bytes, self.owner, self.durable, &c.record)
                    .map_err(error)?
            }
            Some(bytes) => {
                let c: Command = wire::decode(bytes).map_err(error)?;
                if c.version != 1
                    || c.incarnation == 0
                    || c.sequence == 0
                    || (c.rows.is_empty() && c.receipt.is_none())
                {
                    return Err("invalid offline row envelope".into());
                }
                for row in &c.rows {
                    self.check_row(row)?;
                }
                if let Backing::Legacy(d) = &self.backing {
                    return d
                        .apply_gateway(native(p), bytes, &c.rows, c.receipt.as_ref())
                        .map(|_| ())
                        .map_err(error);
                }
                index
                    .gateway(p, bytes, &c.rows, c.receipt.as_ref())
                    .map_err(error)?
            }
        };
        match &self.backing {
            Backing::Rocks(d) => {
                for id in change.table_ids() {
                    d.distributed_table(id.into()).map_err(error)?;
                }
                let rows = change.rows.iter().map(native_row).collect::<Vec<_>>();
                d.apply_distributed(
                    Some(&Prefix {
                        position: native(p),
                        fingerprint: change.fingerprint(),
                    }),
                    &rows,
                    &change.metadata().map_err(error)?,
                )
                .map_err(error)?;
            }
            Backing::Logical(image) => {
                let mut image = image.lock();
                for row in &change.rows {
                    let key = (row.table, row.key.clone());
                    if let Some(old) = image.rows.get(&key) {
                        if old.time_term == row.time_term && old != row {
                            return Err("changed row at existing logical time".into());
                        }
                        if !row.supersedes(old.time_term) {
                            continue;
                        }
                    }
                    image.rows.insert(key, row.clone());
                }
                index.publish(change);
                image.index = index;
            }
            Backing::Legacy(_) => unreachable!(),
        }
        Ok(())
    }
    fn create_checkpoint(&self, path: &Path, p: raft_host::Position) -> Result<()> {
        if self.applied_position()? != p {
            return Err("offline checkpoint cut mismatch".into());
        }
        match &self.backing {
            Backing::Legacy(d) => d.create_checkpoint(path, native(p.into())).map_err(error),
            Backing::Rocks(d) => d
                .create_checkpoint(path, native(p.into()))
                .map(|_| ())
                .map_err(error),
            Backing::Logical(image) => {
                let image = image.lock();
                checkpoint::write(path, self.checkpoint_identity(), &image.index, |visitor| {
                    for row in image.rows.values() {
                        visitor(row.clone())?;
                    }
                    Ok(())
                })
                .map_err(error)
            }
        }
    }
    fn validate_checkpoint(&self, path: &Path, p: raft_host::Position) -> Result<()> {
        match &self.backing {
            Backing::Legacy(d) => d.validate_checkpoint(path, native(p.into())).map_err(error),
            Backing::Rocks(d) => {
                let (p, metadata) = d
                    .distributed_checkpoint_state(path, native(p.into()), &self.identity)
                    .map_err(error)?;
                Index::from_metadata(metadata, position(p).into(), self.owner, self.durable)
                    .map_err(error)?;
                Ok(())
            }
            Backing::Logical(_) => self.load_logical(path, p.into()).map(|_| ()),
        }
    }
    fn publish_checkpoint(&self, path: &Path, p: raft_host::Position) -> Result<()> {
        self.validate_checkpoint(path, p)?;
        match &self.backing {
            Backing::Legacy(d) => d.publish_checkpoint(path, native(p.into())).map_err(error),
            Backing::Rocks(d) => d.publish_checkpoint(path, native(p.into())).map_err(error),
            Backing::Logical(i) => {
                *i.lock() = self.load_logical(path, p.into())?;
                Ok(())
            }
        }
    }
}
fn native_row(row: &Row) -> NativeRow {
    NativeRow {
        table: row.table.into(),
        key: row.key.clone(),
        value: row.value.clone(),
        time_term: row.time_term,
    }
}
fn log_mode(config: &Config) -> Result<(offline::Mode, PathBuf, u64, bool)> {
    if let Some(c) = &config.replication {
        Ok((
            offline::Mode::Replicated(raft_host::Config {
                group: c.group,
                node: c.id,
                listen: c.listen.clone(),
                peers: c
                    .peers
                    .iter()
                    .map(|p| raft_host::Peer {
                        id: p.id,
                        address: p.address.clone(),
                    })
                    .collect(),
                directory: c.directory.join("log"),
                create: false,
                sync_writes: c.sync_writes,
                ..Default::default()
            }),
            c.directory.clone(),
            c.group,
            c.sync_writes,
        ))
    } else if let Some(c) = &config.persistence {
        Ok((
            offline::Mode::Local(raft_host::LocalConfig {
                group: config.owner.into(),
                directory: c.directory.join("log"),
                create: false,
                sync_writes: c.sync_writes,
            }),
            c.directory.clone(),
            config.owner.into(),
            c.sync_writes,
        ))
    } else {
        Err("offline conversion requires a persistent source log".into())
    }
}
#[derive(Debug, Serialize)]
pub struct Report {
    pub source: PathBuf,
    pub destination: PathBuf,
    pub engine: String,
    pub index: u64,
    pub term: u64,
    pub rows: u64,
    pub unresolved: usize,
    pub receipts: usize,
}
/// Convert a stopped shard into a new directory. `rocksdb` chooses its data
/// engine; log mode, membership, durability and protocol identities are retained.
/// The source may replay already committed effects to finish recovery, but no
/// source log command, vote or decision is created. Failed staging is retained.
pub fn convert(config: Config, destination: &Path, rocksdb: bool) -> Result<Report> {
    let schema = config.schema().map_err(error)?;
    let identity = config.storage_identity().map_err(error)?;
    let (mode, source, group, durable) = log_mode(&config)?;
    if source.join("raft").exists() {
        return Err("retired segmented Raft format requires its original format converter".into());
    }
    if !source.join("log/FORMAT").is_file() {
        return Err("source shared log is missing".into());
    }
    let parent = fs::canonicalize(
        destination
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new(".")),
    )
    .map_err(error)?;
    let destination = parent.join(
        destination
            .file_name()
            .ok_or("destination requires a directory name")?,
    );
    if destination.exists() || destination.symlink_metadata().is_ok() {
        return Err("conversion destination already exists".into());
    }
    let source_log = offline::Log::open(mode)?;
    let legacy = source.join("rows").exists();
    if legacy && config.migration {
        return Err(
            "legacy external migration authority cannot be converted as application state".into(),
        );
    }
    let backing = if legacy {
        if !matches!(config.engine, EngineConfig::Masstree) {
            return Err("ambiguous legacy mirror and direct engine configuration".into());
        }
        if !source.join("rows/CURRENT").is_file() {
            return Err("legacy row generation is missing".into());
        }
        Backing::Legacy(
            DurableRows::open(&source.join("rows"), group, durable, &identity).map_err(error)?,
        )
    } else {
        let data = match &config.engine {
            EngineConfig::Masstree => source.join("data"),
            EngineConfig::RocksDb { directory, .. } => directory.clone(),
        };
        if matches!(config.engine, EngineConfig::Masstree) && !data.join("CURRENT").is_file() {
            Backing::Logical(Mutex::new(Image::default()))
        } else {
            if !data.join("CURRENT").is_file() {
                return Err("source RocksDB generation is missing".into());
            }
            let db = Database::open_distributed(&data, u64::from(config.owner), durable)
                .map_err(error)?;
            if db.distributed_identity().map_err(error)?.as_deref() != Some(&identity) {
                return Err("source RocksDB schema mismatch".into());
            }
            Backing::Rocks(db)
        }
    };
    let source_machine = Machine {
        backing,
        owner: config.owner,
        durable,
        identity: identity.clone(),
        schema: schema.clone(),
    };
    if matches!(source_machine.backing, Backing::Logical(_)) {
        source_machine.publish_checkpoint(&source.join("log.bootstrap"), Default::default())?;
    }
    let cut = source_log.recover(&source_machine)?;
    let index = source_machine.index()?;
    index.validate(config.owner, durable).map_err(error)?;
    if index.position != cut.into() {
        return Err("converted index differs from committed cut".into());
    }
    for entry in index.transactions.values() {
        for row in entry
            .participant
            .iter()
            .flat_map(|p| p.rows.iter().flatten())
        {
            source_machine.check_row(row)?;
        }
    }
    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(error)?
        .as_nanos();
    let staging = parent.join(format!(".mako-convert-{}-{nonce:x}", std::process::id()));
    fs::create_dir(&staging).map_err(error)?;
    let db = Database::open_distributed(staging.join("data"), u64::from(config.owner), durable)
        .map_err(error)?;
    db.bind_distributed_identity(&identity).map_err(error)?;
    let target_backing = Backing::Rocks(db);
    let target = Machine {
        backing: target_backing,
        owner: config.owner,
        durable,
        identity,
        schema,
    };
    let mut count = 0u64;
    let mut source_hash = Sha256::new();
    let mut batch = Vec::new();
    let mut tables = std::collections::BTreeSet::new();
    source_machine.visit(|row| {
        source_machine.check_row(&row)?;
        source_hash.update(wire::encode(&row).map_err(error)?);
        match &target.backing {
            Backing::Rocks(d) => {
                if tables.insert(row.table) {
                    d.distributed_table(row.table.into()).map_err(error)?;
                }
                batch.push(native_row(&row));
                if batch.len() == 128 {
                    d.apply_distributed(None, &batch, &Vec::new())
                        .map_err(error)?;
                    batch.clear();
                }
            }
            Backing::Logical(image) => {
                image.lock().rows.insert((row.table, row.key.clone()), row);
            }
            Backing::Legacy(_) => unreachable!(),
        }
        count = count.checked_add(1).ok_or("row count overflow")?;
        Ok(())
    })?;
    if let Backing::Rocks(d) = &target.backing {
        if !batch.is_empty() {
            d.apply_distributed(None, &batch, &Vec::new())
                .map_err(error)?;
        }
    }
    let mut target_hash = Sha256::new();
    let mut target_count = 0u64;
    target.visit(|row| {
        target_hash.update(wire::encode(&row).map_err(error)?);
        target_count += 1;
        Ok(())
    })?;
    if target_count != count || target_hash.finalize() != source_hash.finalize() {
        return Err("converted row digest or count differs from source".into());
    }
    if let Backing::Rocks(d) = &target.backing {
        for row in index
            .transactions
            .values()
            .flat_map(|e| e.participant.iter())
            .flat_map(|p| p.rows.iter().flatten())
        {
            d.distributed_table(row.table.into()).map_err(error)?;
        }
        let history = index
            .fingerprints
            .iter()
            .map(|(id, (term, fingerprint))| Prefix {
                position: sto_rocksdb::Position {
                    index: *id,
                    term: *term,
                },
                fingerprint: *fingerprint,
            })
            .collect::<Vec<_>>();
        d.finish_distributed_import(&history, &index.metadata().map_err(error)?)
            .map_err(error)?;
    }
    source_log.copy_checkpoint(&staging.join("log"), &target)?;
    if wire::encode(&target.index()?).map_err(error)? != wire::encode(&index).map_err(error)? {
        return Err("target protocol index changed during conversion".into());
    }
    let report = Report {
        source: fs::canonicalize(&source).map_err(error)?,
        destination: destination.clone(),
        engine: if rocksdb { "rocksdb" } else { "masstree" }.into(),
        index: cut.index,
        term: cut.term,
        rows: count,
        unresolved: index
            .transactions
            .values()
            .filter(|e| e.unresolved())
            .count(),
        receipts: index.receipts.len(),
    };
    let mut config = config;
    config.engine = EngineConfig::RocksDb {
        directory: destination.join("data"),
        masstree: !rocksdb,
    };
    if let Some(c) = &mut config.persistence {
        c.directory = destination.clone();
        c.create = false;
    }
    if let Some(c) = &mut config.replication {
        c.directory = destination.clone();
        c.create = false;
    }
    write_json(&staging.join("node.json"), &config)?;
    write_json(&staging.join("conversion.json"), &report)?;
    drop(target);
    sync(&staging)?;
    publish(&staging, &destination)?;
    sync(&parent)?;
    Ok(report)
}
fn write_json(path: &Path, value: &impl Serialize) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(error)?;
    file.write_all(&serde_json::to_vec_pretty(value).map_err(error)?)
        .map_err(error)?;
    file.write_all(b"\n").map_err(error)?;
    file.sync_all().map_err(error)
}
fn publish(source: &Path, destination: &Path) -> Result<()> {
    use std::os::unix::ffi::OsStrExt;
    let source = std::ffi::CString::new(source.as_os_str().as_bytes()).map_err(error)?;
    let destination = std::ffi::CString::new(destination.as_os_str().as_bytes()).map_err(error)?;
    // SAFETY: both C strings remain alive for the synchronous Linux syscall.
    // NOREPLACE prevents a racing converter from overwriting a published tree.
    let rc = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            source.as_ptr(),
            libc::AT_FDCWD,
            destination.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if rc != 0 {
        return Err(error(std::io::Error::last_os_error()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::migration::{Kind, TableSpec};
    use mako_protocol::journal::{self, Member, Record, Transaction, TransactionId};
    use raft_host::{LocalConfig, LocalHost, ProposalStatus};
    use std::time::Duration;
    fn config(root: &Path) -> Config {
        Config {
            engine: EngineConfig::Masstree,
            owner: 0,
            nodes: 1,
            listen: "127.0.0.1:0".into(),
            tables: vec![TableSpec {
                id: 1,
                name: "rows".into(),
                kind: Kind::Governed,
                warehouse: false,
                initial_owner: 0,
            }],
            workers: 1,
            gateway_workers: 1,
            queue_capacity: 16,
            warehouses_per_shard: 0,
            peers: BTreeMap::new(),
            migration: false,
            initial: Vec::new(),
            replication: None,
            persistence: Some(crate::node::PersistenceConfig {
                directory: root.into(),
                create: false,
                sync_writes: true,
            }),
            volatile: false,
            replica_groups: BTreeMap::new(),
        }
    }
    fn legacy(root: &Path) -> (Config, Arc<Machine>, LocalHost) {
        let config = config(root);
        let identity = config.storage_identity().unwrap();
        fs::create_dir(root).unwrap();
        let db = DurableRows::open(&root.join("rows"), 0, true, &identity).unwrap();
        let machine = Arc::new(Machine {
            backing: Backing::Legacy(db),
            owner: 0,
            durable: true,
            identity,
            schema: config.schema().unwrap(),
        });
        let host = LocalHost::start(
            LocalConfig {
                group: 0,
                directory: root.join("log"),
                create: true,
                sync_writes: true,
            },
            machine.clone(),
        )
        .unwrap();
        (config, machine, host)
    }
    fn propose(host: &LocalHost, bytes: Vec<u8>) {
        assert!(matches!(
            host.propose(1, bytes).unwrap().wait(Duration::from_secs(5)),
            ProposalStatus::Applied
        ));
    }
    fn row(key: &[u8], value: Option<&[u8]>, time: u32) -> Row {
        Row {
            table: 1,
            key: key.into(),
            value: value.map(Vec::from),
            time_term: time,
        }
    }
    fn fixture(config: &Config, host: &LocalHost) -> Transaction {
        let receipt = wire::GatewayReceipt {
            request: wire::GatewayRequest {
                client: 1 << 63,
                sequence: 1,
                session: 1,
                kind: 23,
                target: Some(wire::Target {
                    owner: 0,
                    access: wire::Access {
                        table: wire::TableKey {
                            table: 1,
                            warehouse: 0,
                        },
                        route: None,
                    },
                    log_id: 1,
                }),
                key: b"live".to_vec(),
                value: b"first".to_vec(),
                forwarded: false,
            },
            response: wire::GatewayResponse {
                client: 1 << 63,
                sequence: 1,
                snapshot_version: 0,
                status: 0,
                outcome: 1,
                op_result: true,
                value: Vec::new(),
            },
        };
        propose(
            host,
            wire::encode(&Command {
                version: 1,
                incarnation: 1,
                sequence: 1,
                rows: vec![
                    row(b"live", Some(b"first"), 20),
                    row(b"gone", Some(b"old"), 20),
                ],
                receipt: Some(receipt),
            })
            .unwrap(),
        );
        let transaction = Transaction {
            version: journal::VERSION,
            id: TransactionId {
                coordinator: 0,
                worker: 0,
                incarnation: 9,
                attempt: 1,
            },
            time_term: 40,
            durable: true,
            members: vec![Member {
                owner: 0,
                endpoints: vec![config.listen.clone()],
                lease: 1,
            }],
        };
        for (i, record) in [
            Record::Begin(transaction.clone()),
            Record::Prepare {
                transaction: transaction.clone(),
                rows: vec![row(b"pending", Some(b"invisible"), 40)],
                receipt: None,
            },
        ]
        .into_iter()
        .enumerate()
        {
            propose(
                host,
                wire::encode(&JournalCommand {
                    version: 2,
                    incarnation: 2,
                    sequence: i as u64 + 1,
                    record,
                })
                .unwrap(),
            );
        }
        host.checkpoint().unwrap();
        propose(
            host,
            wire::encode(&Command {
                version: 1,
                incarnation: 1,
                sequence: 2,
                rows: vec![row(b"live", Some(b"suffix"), 30), row(b"gone", None, 30)],
                receipt: None,
            })
            .unwrap(),
        );
        transaction
    }
    fn open_target(config: &Config) -> (offline::Log, Machine) {
        let (mode, root, _group, durable) = log_mode(config).unwrap();
        let log = offline::Log::open(mode).unwrap();
        let identity = config.storage_identity().unwrap();
        let backing = match &config.engine {
            EngineConfig::Masstree => Backing::Logical(Mutex::new(Image::default())),
            EngineConfig::RocksDb { directory, .. } => Backing::Rocks(
                Database::open_distributed(directory, u64::from(config.owner), durable).unwrap(),
            ),
        };
        let machine = Machine {
            backing,
            owner: config.owner,
            durable,
            identity,
            schema: config.schema().unwrap(),
        };
        if matches!(machine.backing, Backing::Logical(_)) {
            machine
                .publish_checkpoint(&root.join("log.bootstrap"), Default::default())
                .unwrap();
        }
        log.recover(&machine).unwrap();
        (log, machine)
    }
    #[test]
    fn legacy_trimmed_log_converts_both_engines_and_roundtrips_prepares_receipts_tombstones() {
        let dir = tempfile::tempdir().unwrap();
        let (config, source, host) = legacy(&dir.path().join("source"));
        let transaction = fixture(&config, &host);
        let expected = wire::encode(&source.index().unwrap()).unwrap();
        let cut = source.applied_position().unwrap();
        host.shutdown();
        drop(host);
        drop(source);
        for (i, rocks) in [false, true].into_iter().enumerate() {
            let output = dir.path().join(format!("converted-{i}"));
            let report = convert(config.clone(), &output, rocks).unwrap();
            assert_eq!(
                (
                    report.index,
                    report.term,
                    report.rows,
                    report.unresolved,
                    report.receipts
                ),
                (cut.index, cut.term, 2, 1, 1)
            );
            assert!(!output.join("rows").exists());
            let new_config: Config = crate::configuration::read(output.join("node.json")).unwrap();
            {
                let (_log, machine) = open_target(&new_config);
                assert_eq!(wire::encode(&machine.index().unwrap()).unwrap(), expected);
                let mut rows = Vec::new();
                machine
                    .visit(|r| {
                        rows.push(r);
                        Ok(())
                    })
                    .unwrap();
                assert_eq!(
                    rows,
                    vec![row(b"gone", None, 30), row(b"live", Some(b"suffix"), 30)]
                );
                assert!(machine.index().unwrap().transactions[&transaction.id].unresolved());
            }
            // Engine conversion must work when every source log command was
            // trimmed and only the published checkpoint retains protocol state.
            let second = dir.path().join(format!("roundtrip-{i}"));
            convert(new_config, &second, !rocks).unwrap();
            let cfg: Config = crate::configuration::read(second.join("node.json")).unwrap();
            let (_log, machine) = open_target(&cfg);
            assert_eq!(wire::encode(&machine.index().unwrap()).unwrap(), expected);
        }
        let (mode, _, _, _) = log_mode(&config).unwrap();
        assert_eq!(
            offline::Log::open(mode).unwrap().committed(),
            cut,
            "source log authority retained"
        );
    }
    #[test]
    fn file_only_masstree_checkpoint_upgrades_to_common_rocks_format() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("file-only");
        fs::create_dir(&root).unwrap();
        let config = config(&root);
        let machine = Arc::new(Machine {
            backing: Backing::Logical(Mutex::new(Image::default())),
            owner: config.owner,
            durable: true,
            identity: config.storage_identity().unwrap(),
            schema: config.schema().unwrap(),
        });
        machine
            .create_checkpoint(&root.join("log.bootstrap"), Default::default())
            .unwrap();
        let host = LocalHost::start(
            LocalConfig {
                group: 0,
                directory: root.join("log"),
                create: true,
                sync_writes: true,
            },
            machine.clone(),
        )
        .unwrap();
        fixture(&config, &host);
        let expected = wire::encode(&machine.index().unwrap()).unwrap();
        host.shutdown();
        drop(host);
        drop(machine);
        let before = fs::read(root.join("log.bootstrap/MANIFEST")).unwrap();
        let refusal = crate::node::RunningNode::start(config.clone())
            .err()
            .expect("old format must be refused");
        assert!(refusal.message.contains("offline conversion"));
        assert!(!root.join("data").exists());
        assert_eq!(
            fs::read(root.join("log.bootstrap/MANIFEST")).unwrap(),
            before
        );
        for rocksdb in [false, true] {
            let destination = dir.path().join(format!("upgraded-{rocksdb}"));
            convert(config.clone(), &destination, rocksdb).unwrap();
            assert!(destination.join("data/CURRENT").is_file());
            assert!(!destination.join("log.bootstrap").exists());
            let cfg: Config = crate::configuration::read(destination.join("node.json")).unwrap();
            let (_, machine) = open_target(&cfg);
            assert_eq!(wire::encode(&machine.index().unwrap()).unwrap(), expected);
        }
    }
    #[test]
    fn conversion_refuses_live_source_existing_target_and_corrupt_checkpoint() {
        let dir = tempfile::tempdir().unwrap();
        let (config, machine, host) = legacy(&dir.path().join("source"));
        fixture(&config, &host);
        let destination = dir.path().join("new");
        assert!(convert(config.clone(), &destination, false).is_err());
        assert!(!destination.exists());
        host.shutdown();
        drop(host);
        drop(machine);
        convert(config.clone(), &destination, false).unwrap();
        let manifest = fs::read(destination.join("conversion.json")).unwrap();
        assert!(convert(config, &destination, true)
            .unwrap_err()
            .contains("already exists"));
        assert_eq!(
            fs::read(destination.join("conversion.json")).unwrap(),
            manifest
        );
        let cfg: Config = crate::configuration::read(destination.join("node.json")).unwrap();
        let generation = fs::read_dir(destination.join("log/checkpoints"))
            .unwrap()
            .next()
            .unwrap()
            .unwrap()
            .path();
        fs::write(generation.join("STO-CHECKPOINT"), b"corrupt").unwrap();
        let bad = dir.path().join("bad");
        assert!(convert(cfg, &bad, true).is_err());
        assert!(!bad.exists());
    }
}
