//! Stable logical redo records, independent of native WriteBatch layout.
use crate::{Error, Result};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const MAX_COMMAND_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_KEY_BYTES: usize = 1024 * 1024;
pub const MAX_OPERATIONS: usize = 100_000;
const MAGIC: &[u8; 8] = b"STORDB01";

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct Position { pub index: u64, pub term: u64 }
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct RequestId { pub client: u64, pub sequence: u64 }
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Mutation {
    pub table: u64,
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    pub generation: u64,
    pub was_present: bool,
}

/// A transient encoding view into sealed core intents, never another owner.
#[derive(Clone, Copy)]
pub(crate) struct MutationRef<'a> {
    pub table: u64,
    pub key: &'a [u8],
    pub value: Option<&'a [u8]>,
    pub generation: u64,
    pub was_present: bool,
}
impl<'a> From<&'a Mutation> for MutationRef<'a> {
    fn from(mutation: &'a Mutation) -> Self {
        Self { table: mutation.table, key: &mutation.key, value: mutation.value.as_deref(),
            generation: mutation.generation, was_present: mutation.was_present }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CommandBody {
    Transaction { mutations: Vec<Mutation>, count_deltas: BTreeMap<u64, i64> },
    CreateTable { name: String },
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Command { pub group: u64, pub request: RequestId, pub body: CommandBody }
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum OutcomeKind { Transaction, Table(u64) }
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Outcome { pub request: RequestId, pub position: Position, pub kind: OutcomeKind }
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ApplyOutcome { pub outcome: Option<Outcome>, pub duplicate: bool, pub applied: Position }

impl Command {
    pub fn create_table(group: u64, request: RequestId, name: String) -> Result<Self> {
        let command = Self { group, request, body: CommandBody::CreateTable { name } };
        command.validate()?;
        Ok(command)
    }
    pub fn validate(&self) -> Result<()> {
        match &self.body {
            CommandBody::CreateTable { name } => {
                if name.is_empty() || name.len() > 4096 { return Err(Error::InvalidCommand("table name length")); }
            }
            CommandBody::Transaction { mutations, count_deltas } => {
                transaction_layout(mutations.iter().map(MutationRef::from), Some(count_deltas))?;
            }
        }
        Ok(())
    }
    pub fn encode(&self) -> Result<Vec<u8>> {
        match &self.body {
            CommandBody::CreateTable { name } => {
                self.validate()?;
                let mut out = encode_header(self.group, self.request, 37 + name.len());
                out.push(1);
                put_bytes(&mut out, name.as_bytes())?;
                Ok(out)
            }
            CommandBody::Transaction { mutations, count_deltas } =>
                encode_transaction(self.group, self.request,
                    mutations.iter().map(MutationRef::from), Some(count_deltas)),
        }
    }
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() > MAX_COMMAND_BYTES { return Err(Error::TooLarge); }
        let mut input = Decoder(bytes);
        if input.take(8)? != MAGIC { return Err(Error::InvalidCommand("format version")); }
        let group = input.u64()?;
        let request = RequestId { client: input.u64()?, sequence: input.u64()? };
        let body = match input.byte()? {
            1 => CommandBody::CreateTable { name: String::from_utf8(input.bytes()?.to_vec()).map_err(|_| Error::InvalidCommand("table name UTF-8"))? },
            2 => {
                let count = input.u32()? as usize;
                if count == 0 || count > MAX_OPERATIONS || count > input.0.len() / 22 { return Err(Error::InvalidCommand("mutation count")); }
                let mut mutations = Vec::with_capacity(count);
                for _ in 0..count {
                    let table = input.u64()?;
                    let generation = input.u64()?;
                    let was_present = input.boolean()?;
                    let key = input.bytes()?.to_vec();
                    let value = if input.boolean()? { Some(input.bytes()?.to_vec()) } else { None };
                    mutations.push(Mutation { table, key, value, generation, was_present });
                }
                let count = input.u32()? as usize;
                if count > mutations.len() { return Err(Error::InvalidCommand("count delta count")); }
                let mut count_deltas = BTreeMap::new();
                let mut previous = 0;
                for _ in 0..count {
                    let table = input.u64()?;
                    let delta = i64::from_be_bytes(input.take(8)?.try_into().unwrap());
                    if table <= previous || delta == 0 { return Err(Error::InvalidCommand("noncanonical deltas")); }
                    previous = table;
                    count_deltas.insert(table, delta);
                }
                CommandBody::Transaction { mutations, count_deltas }
            }
            _ => return Err(Error::InvalidCommand("command tag")),
        };
        if !input.0.is_empty() { return Err(Error::InvalidCommand("trailing bytes")); }
        let command = Self { group, request, body };
        command.validate()?;
        Ok(command)
    }
}

/// Encode once from borrowed, canonical records. Only the descriptor sort
/// vector and scalar table deltas are transient; key/value payloads stay in STO.
pub(crate) fn encode_transaction<'a>(
    group: u64,
    request: RequestId,
    mutations: impl ExactSizeIterator<Item = MutationRef<'a>> + Clone,
    expected_deltas: Option<&BTreeMap<u64, i64>>,
) -> Result<Vec<u8>> {
    let (deltas, length) = transaction_layout(mutations.clone(), expected_deltas)?;
    let mut out = encode_header(group, request, length);
    out.push(2);
    put_u32(&mut out, mutations.len())?;
    for mutation in mutations {
        put_u64(&mut out, mutation.table);
        put_u64(&mut out, mutation.generation);
        out.push(u8::from(mutation.was_present));
        put_bytes(&mut out, mutation.key)?;
        out.push(u8::from(mutation.value.is_some()));
        if let Some(value) = mutation.value { put_bytes(&mut out, value)?; }
    }
    put_u32(&mut out, deltas.len())?;
    for (table, delta) in deltas {
        put_u64(&mut out, table);
        out.extend_from_slice(&delta.to_be_bytes());
    }
    Ok(out)
}

fn encode_header(group: u64, request: RequestId, capacity: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(capacity);
    out.extend_from_slice(MAGIC);
    put_u64(&mut out, group);
    put_u64(&mut out, request.client);
    put_u64(&mut out, request.sequence);
    out
}

fn transaction_layout<'a>(
    mutations: impl ExactSizeIterator<Item = MutationRef<'a>>,
    expected_deltas: Option<&BTreeMap<u64, i64>>,
) -> Result<(BTreeMap<u64, i64>, usize)> {
    if mutations.len() == 0 || mutations.len() > MAX_OPERATIONS {
        return Err(Error::InvalidCommand("mutation count"));
    }
    let mut previous = None;
    let mut deltas = BTreeMap::<u64, i64>::new();
    // Header, command tag, mutation count and count-delta count.
    let mut length = 41usize;
    for mutation in mutations {
        let key = (mutation.table, mutation.key);
        if mutation.table == 0 || mutation.generation == 0
            || mutation.key.len() > MAX_KEY_BYTES
            || previous.is_some_and(|previous| previous >= key) {
            return Err(Error::InvalidCommand("noncanonical mutation"));
        }
        previous = Some(key);
        let value_length = mutation.value.map_or(Some(0), |value| value.len().checked_add(4))
            .ok_or(Error::TooLarge)?;
        length = length.checked_add(22).and_then(|size| size.checked_add(mutation.key.len()))
            .and_then(|size| size.checked_add(value_length)).ok_or(Error::TooLarge)?;
        if length > MAX_COMMAND_BYTES { return Err(Error::TooLarge); }
        *deltas.entry(mutation.table).or_default() +=
            i64::from(mutation.value.is_some()) - i64::from(mutation.was_present);
    }
    deltas.retain(|_, delta| *delta != 0);
    if expected_deltas.is_some_and(|expected| expected != &deltas) {
        return Err(Error::InvalidCommand("count delta mismatch"));
    }
    length = length.checked_add(deltas.len().checked_mul(16).ok_or(Error::TooLarge)?)
        .ok_or(Error::TooLarge)?;
    if length > MAX_COMMAND_BYTES { return Err(Error::TooLarge); }
    Ok((deltas, length))
}
pub(crate) fn digest(bytes: &[u8]) -> [u8; 32] { Sha256::digest(bytes).into() }
pub(crate) fn put_u64(out: &mut Vec<u8>, value: u64) { out.extend_from_slice(&value.to_be_bytes()); }
fn put_u32(out: &mut Vec<u8>, value: usize) -> Result<()> {
    out.extend_from_slice(&u32::try_from(value).map_err(|_| Error::TooLarge)?.to_be_bytes()); Ok(())
}
fn put_bytes(out: &mut Vec<u8>, bytes: &[u8]) -> Result<()> {
    if out.len().checked_add(4).and_then(|size| size.checked_add(bytes.len()))
        .is_none_or(|size| size > MAX_COMMAND_BYTES) { return Err(Error::TooLarge); }
    put_u32(out, bytes.len())?; out.extend_from_slice(bytes); Ok(())
}
pub(crate) struct Decoder<'a>(pub &'a [u8]);
impl<'a> Decoder<'a> {
    pub fn take(&mut self, length: usize) -> Result<&'a [u8]> {
        if length > self.0.len() { return Err(Error::InvalidCommand("truncated bytes")); }
        let (value, remaining) = self.0.split_at(length); self.0 = remaining; Ok(value)
    }
    pub fn byte(&mut self) -> Result<u8> { Ok(self.take(1)?[0]) }
    pub fn boolean(&mut self) -> Result<bool> { match self.byte()? { 0 => Ok(false), 1 => Ok(true), _ => Err(Error::InvalidCommand("boolean")) } }
    pub fn u64(&mut self) -> Result<u64> { Ok(u64::from_be_bytes(self.take(8)?.try_into().unwrap())) }
    pub fn u32(&mut self) -> Result<u32> { Ok(u32::from_be_bytes(self.take(4)?.try_into().unwrap())) }
    pub fn bytes(&mut self) -> Result<&'a [u8]> { let length = self.u32()? as usize; self.take(length) }
}
