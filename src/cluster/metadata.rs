//! Persistent live bookkeeping, deliberately NOT DurableParticipant recovery.
//! Completion records describe observed live effects, not recoverable row data.
//! Reopening validates the retained prefix but never reactivates a participant.
use std::path::{Path, PathBuf};
use crate::durable_codec::{write_grant, write_txn, read_grant, read_txn};
use crate::durable_kv::{Journal, Namespace};
use crate::participant::{Cleanup, ControlResult};
use crate::rocks_metadata::RocksKv;
use crate::routing_codec::{Reader, Writer};
use crate::types::{Grant, Status, TxnId};
use crate::wire::{self, checked};

// Separate payload version from the durable-engine participant codec: a live
// receipt must never be mistaken for a durable row completion during recovery.
const TAG: u64 = 0x014d42444f4b414d;

pub struct Configuration {
    pub directory: Option<PathBuf>,
    pub initialize: bool,
}
impl Configuration {
    pub fn load(owner: u32) -> Result<Self, Status> {
        let directory = std::env::var_os("MAKO_SHARD_METADATA_DIR");
        let initialize = match std::env::var_os("MAKO_SHARD_METADATA_INIT") {
            None => false,
            Some(value) if value == "1" => true,
            Some(value) if value == "0" => false,
            Some(_) => return Err(Status::Invalid),
        };
        if initialize && directory.is_none() { return Err(Status::Invalid); }
        let directory = match directory {
            Some(path) if !path.is_empty() => Some(PathBuf::from(path).join(format!("owner-{owner}"))),
            Some(_) => return Err(Status::Invalid),
            None => None,
        };
        Ok(Self { directory, initialize })
    }
    pub fn path(&self, role: &str) -> Option<PathBuf> {
        self.directory.as_ref().map(|path| path.join(role))
    }
}

pub enum Event<'a> {
    Begin(TxnId),
    Acquire { id: TxnId, table: u64, coordinate: &'a [u8], grant: Grant },
    Range { id: TxnId, table: u64, lo: &'a [u8], hi: Option<&'a [u8]>, grant: Grant },
    Finish(TxnId),
    Control { operation: u32, plan: &'a [u8], result: &'a ControlResult },
    Transfer { plan: &'a [u8], cleanup: bool, result: Option<&'a ControlResult> },
}

struct Persistent { kv: RocksKv, journal: Journal }
pub struct ParticipantLog {
    persistent: Option<Persistent>,
    recovered: bool,
    failed: bool,
}
impl ParticipantLog {
    pub fn new(owner: u32, nodes: u32, identity: &[u8], path: Option<&Path>, initialize: bool)
        -> Result<Self, Status>
    {
        let Some(path) = path else {
            return Ok(Self { persistent: None, recovered: false, failed: false });
        };
        let mut kv = RocksKv::open(path, initialize)?;
        let namespace = Namespace { owner, coordinator: false };
        let mut bootstrap = header(0)?;
        checked(bootstrap.write_u32(owner))?;
        checked(bootstrap.write_bytes(identity))?;
        let bootstrap = bootstrap.into_bytes();
        let mut journal = if initialize {
            let mut journal = Journal::create(&mut kv, namespace, bootstrap).map_err(|_| Status::Io)?;
            journal.activate(&mut kv).map_err(|_| Status::Io)?;
            journal
        } else {
            let mut journal = Journal::recover(&mut kv, namespace).map_err(|_| Status::Io)?;
            if journal.read_record(&mut kv, 0).map_err(|_| Status::Io)? != bootstrap {
                return Err(Status::Invalid);
            }
            // Retain an inactive journal. Syntax/identity validation is not a
            // claim that the lost row image or old TP work can be reconstructed.
            for index in 1..journal.count() {
                let record = journal.read_record(&mut kv, index).map_err(|_| Status::Io)?;
                validate(&record, nodes)?;
            }
            journal
        };
        if !initialize { journal.fence(); }
        Ok(Self { persistent: Some(Persistent { kv, journal }), recovered: !initialize, failed: false })
    }
    pub fn enabled(&self) -> bool { self.persistent.is_some() }
    pub fn recovered(&self) -> bool { self.recovered }
    pub fn failed(&self) -> bool { self.failed }
    pub fn record(&mut self, event: Event<'_>) -> Result<(), Status> {
        if self.failed { return Err(Status::Io); }
        if self.recovered { return Err(Status::Retry); }
        let Some(persistent) = &mut self.persistent else { return Ok(()); };
        // A failed encoding is also fatal here: the caller may already have
        // changed private live metadata. It must not publish that candidate.
        let result = encode(event).and_then(|record| {
            persistent.journal.append(&mut persistent.kv, record).map_err(|_| Status::Io)
        });
        if result.is_err() {
            persistent.journal.fence();
            self.failed = true;
        }
        result
    }
}

fn header(kind: u32) -> Result<Writer, Status> {
    let mut writer = Writer::new();
    checked(writer.write_u64(TAG))?;
    checked(writer.write_u32(kind))?;
    Ok(writer)
}
fn write_result(writer: &mut Writer, result: &ControlResult) -> Result<(), Status> {
    checked(writer.write_u32(result.status as u32))?;
    checked(writer.write_u32(result.certificate.map_or(0, |certificate| certificate as u32 + 1)))?;
    checked(writer.write_u32(match result.cleanup {
        Cleanup::None => 0, Cleanup::Source => 1, Cleanup::Destination => 2,
    }))
}
fn encode(event: Event<'_>) -> Result<Vec<u8>, Status> {
    let kind = match &event {
        Event::Begin(_) => 1, Event::Acquire { .. } => 2, Event::Range { .. } => 3,
        Event::Finish(_) => 4, Event::Control { .. } => 5,
        Event::Transfer { result: None, .. } => 6,
        Event::Transfer { result: Some(_), .. } => 7,
    };
    let mut writer = header(kind)?;
    match event {
        Event::Begin(id) | Event::Finish(id) => checked(write_txn(&mut writer, id))?,
        Event::Acquire { id, table, coordinate, grant } => {
            checked(write_txn(&mut writer, id))?;
            checked(writer.write_u64(table))?;
            checked(writer.write_bytes(coordinate))?;
            checked(write_grant(&mut writer, grant))?;
        },
        Event::Range { id, table, lo, hi, grant } => {
            checked(write_txn(&mut writer, id))?;
            checked(writer.write_u64(table))?;
            checked(writer.write_bytes(lo))?;
            checked(writer.write_u32(u32::from(hi.is_some())))?;
            if let Some(hi) = hi { checked(writer.write_bytes(hi))?; }
            checked(write_grant(&mut writer, grant))?;
        },
        Event::Control { operation, plan, result } => {
            checked(writer.write_u32(operation))?;
            checked(writer.write_bytes(plan))?;
            write_result(&mut writer, result)?;
        },
        Event::Transfer { plan, cleanup, result } => {
            checked(writer.write_bytes(plan))?;
            checked(writer.write_u32(u32::from(cleanup)))?;
            if let Some(result) = result { write_result(&mut writer, result)?; }
        },
    }
    Ok(writer.into_bytes())
}
fn blob<'a>(reader: &mut Reader<'a>) -> Result<&'a [u8], Status> {
    let length = usize::try_from(reader.read_u64()?).map_err(|_| Status::Invalid)?;
    reader.read_slice(length)
}
fn validate_result(reader: &mut Reader<'_>) -> Result<(), Status> {
    if reader.read_u32()? > Status::Io as u32 || reader.read_u32()? > 5 || reader.read_u32()? > 2 {
        return Err(Status::Invalid);
    }
    Ok(())
}
fn validate(bytes: &[u8], nodes: u32) -> Result<(), Status> {
    let mut reader = Reader::new(bytes);
    if reader.read_u64()? != TAG { return Err(Status::Invalid); }
    let kind = reader.read_u32()?;
    match kind {
        1 | 4 => { read_txn(&mut reader)?; },
        2 | 3 => {
            read_txn(&mut reader)?;
            reader.read_u64()?;
            blob(&mut reader)?;
            if kind == 3 {
                match reader.read_u32()? {
                    0 => {}, 1 => { blob(&mut reader)?; }, _ => return Err(Status::Invalid),
                }
            }
            if read_grant(&mut reader)?.owner >= nodes { return Err(Status::Invalid); }
        },
        5 => {
            let operation = reader.read_u32()?;
            if wire::command(operation).is_none() && operation != wire::DRAIN {
                return Err(Status::Invalid);
            }
            wire::decode_plan(blob(&mut reader)?, nodes)?;
            validate_result(&mut reader)?;
        },
        6 | 7 => {
            wire::decode_plan(blob(&mut reader)?, nodes)?;
            if reader.read_u32()? > 1 { return Err(Status::Invalid); }
            if kind == 7 { validate_result(&mut reader)?; }
        },
        _ => return Err(Status::Invalid),
    }
    checked(reader.finish())
}
