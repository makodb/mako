//! Storage effects decided by an external distributed transaction protocol.
//!
//! Native conflict generations and distributed logical times are independent.
//! Rows, tombstones, protocol metadata and the applied log prefix share one WAL
//! batch. The caller owns decision authority; this module never revalidates it.
use crate::database::*;
use crate::transaction::Key;
use crate::{
    mutex, Database, Error, Position, Result, Table, MAX_COMMAND_BYTES, MAX_KEY_BYTES,
    MAX_OPERATIONS,
};
use mako_rocksdb_store::{BlobOp, Blobs};
use std::collections::{BTreeMap, BTreeSet};
use std::sync::{atomic::Ordering, Weak};

pub(crate) const PROFILE: &[u8] = &[0, 11];
pub(crate) const PROFILE_VALUE: &[u8] = b"DISTRIBUTED-STO-01";
pub(crate) const IDENTITY: &[u8] = &[0, 12];
const CLOCK: &[u8] = &[0, 13];
const METADATA: &[u8] = &[0, 10];
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Row {
    pub table: u64,
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
#[derive(Clone, Debug)]
pub struct Prefix {
    pub position: Position,
    pub fingerprint: [u8; 32],
}
pub type Metadata = Vec<(Vec<u8>, Vec<u8>)>;
/// Consecutive, already decided log effects. One native batch publishes every
/// effect and its replay identity, ending at the final position. The caller
/// excludes application readers while publication is in progress.
pub struct Batch<'a> {
    pub prefix: &'a Prefix,
    pub rows: &'a [Row],
    pub metadata: &'a Metadata,
}
struct Input<'a> {
    prefix: Option<&'a Prefix>,
    rows: &'a [Row],
    metadata: &'a Metadata,
}

pub(crate) fn time_key(table: u64, key: &[u8]) -> Vec<u8> {
    let mut result = vec![0, 9];
    result.extend_from_slice(&table.to_be_bytes());
    result.extend_from_slice(key);
    result
}
pub(crate) fn decode_time(value: Option<Vec<u8>>) -> Result<u32> {
    value.map_or(Ok(0), |v| {
        Ok(u32::from_be_bytes(v.as_slice().try_into().map_err(
            |_| Error::Corruption("logical timestamp length".into()),
        )?))
    })
}
impl Database {
    pub(crate) fn ensure_distributed(&self) -> Result<()> {
        self.ensure_serving()?;
        if !self.distributed {
            return Err(Error::InvalidState("not a distributed database"));
        }
        Ok(())
    }
    /// Bind the schema/owner identity before opening admission. Never upgrades
    /// an already identified database to another schema.
    pub fn bind_distributed_identity(&self, identity: &[u8]) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        let rocks = self.rocks();
        match rocks.get(IDENTITY)? {
            Some(old) if old != identity => Err(Error::Corruption(
                "distributed schema identity mismatch".into(),
            )),
            Some(_) => Ok(()),
            None => {
                rocks.write_batch(&[BlobOp::Put {
                    key: IDENTITY,
                    val: identity,
                }])?;
                Ok(())
            }
        }
    }
    pub fn distributed_identity(&self) -> Result<Option<Vec<u8>>> {
        self.ensure_distributed()?;
        Ok(self.rocks().get(IDENTITY)?)
    }
    /// Stable logical table IDs do not consume transaction-log positions.
    pub fn distributed_table(&self, id: u64) -> Result<Table> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        if id == 0 {
            return Err(Error::InvalidCommand("zero table ID"));
        }
        let rocks = self.rocks();
        let name = format!("distributed-{id}");
        let key = table_key(id);
        if let Some(existing) = rocks.get(&key)? {
            if existing != name.as_bytes() {
                return Err(Error::Corruption("table identity mismatch".into()));
            }
        } else {
            rocks.write_batch(&[
                BlobOp::Put {
                    key: &key,
                    val: name.as_bytes(),
                },
                BlobOp::Put {
                    key: &catalog_key(&name),
                    val: &id.to_be_bytes(),
                },
                BlobOp::Put {
                    key: &count_key(id),
                    val: &0u64.to_be_bytes(),
                },
            ])?;
        }
        Ok(Table { id, name })
    }
    pub fn distributed_metadata(&self) -> Result<Metadata> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        read_metadata(&self.rocks())
    }
    pub fn distributed_durable(&self) -> bool {
        self.sync_writes
    }

    pub fn distributed_clock(&self) -> Result<u32> {
        self.ensure_distributed()?;
        decode_time(self.rocks().get(CLOCK)?)
    }
    /// `None` is reserved for bootstrap, logical import, and MemoryLog tests.
    /// Production decisions include their authoritative log position and digest.
    /// On any ambiguous batch error the database fences until reopened.
    pub fn apply_distributed(
        &self,
        prefix: Option<&Prefix>,
        rows: &[Row],
        metadata: &Metadata,
    ) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        let result = self.apply_distributed_locked(&[Input {
            prefix,
            rows,
            metadata,
        }]);
        if result.is_err() {
            self.fence();
        }
        result
    }
    pub fn apply_distributed_batch(&self, batches: &[Batch<'_>]) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        let inputs = batches
            .iter()
            .map(|b| Input {
                prefix: Some(b.prefix),
                rows: b.rows,
                metadata: b.metadata,
            })
            .collect::<Vec<_>>();
        let result = self.apply_distributed_locked(&inputs);
        if result.is_err() {
            self.fence();
        }
        result
    }
    fn apply_distributed_locked(&self, inputs: &[Input<'_>]) -> Result<()> {
        if inputs.is_empty() || inputs.len() > 4096 {
            return Err(Error::InvalidCommand(
                "empty or oversized application batch",
            ));
        }
        let total_rows = inputs
            .iter()
            .try_fold(0usize, |n, input| n.checked_add(input.rows.len()))
            .ok_or(Error::TooLarge)?;
        if total_rows > MAX_OPERATIONS {
            return Err(Error::TooLarge);
        }
        let rocks = self.rocks();
        let mut batch = Vec::<(Vec<u8>, Option<Vec<u8>>)>::new();
        let old = read_position(&rocks)?;
        let mut previous: Option<Position> = None;
        let mut duplicate = None;
        for input in inputs {
            let Some(prefix) = input.prefix else {
                if inputs.len() != 1 {
                    return Err(Error::InvalidCommand(
                        "bootstrap mixed with positioned effects",
                    ));
                }
                continue;
            };
            let position = prefix.position;
            if position.index == 0 || position.term == 0 {
                return Err(Error::InvalidCommand("zero log position"));
            }
            if let Some(previous) = previous {
                if previous.index.checked_add(1) != Some(position.index)
                    || position.term < previous.term
                {
                    return Err(Error::Corruption("noncontiguous application batch".into()));
                }
            }
            let replay = position.index <= old.index;
            if duplicate.is_some_and(|prior| prior != replay) {
                return Err(Error::Corruption(
                    "partially overlapping application batch".into(),
                ));
            }
            duplicate = Some(replay);
            let mut receipt = position.term.to_be_bytes().to_vec();
            receipt.extend_from_slice(&prefix.fingerprint);
            if replay {
                if rocks.get(&entry_key(position.index))?.as_deref() != Some(receipt.as_slice()) {
                    return Err(Error::Corruption("conflicting external log prefix".into()));
                }
            } else {
                let prior = previous.unwrap_or(old);
                let expected = prior.index.checked_add(1).ok_or(Error::VersionExhausted)?;
                if position.index != expected {
                    return Err(Error::AppliedGap {
                        expected,
                        received: position.index,
                    });
                }
                if position.term < prior.term {
                    return Err(Error::Corruption("regressing external log term".into()));
                }
                batch.push((entry_key(position.index), Some(receipt)));
            }
            previous = Some(position);
        }
        if duplicate == Some(true) {
            return Ok(());
        }
        if let Some(position) = previous {
            batch.push((APPLIED.to_vec(), Some(position_bytes(position))));
        }
        // Rows may recur in successive commands. Validate against the preceding
        // command's result, not a stale read of the pre-batch database.
        let mut shadow = BTreeMap::<(u64, Vec<u8>), (Option<Vec<u8>>, u32, u64)>::new();
        let mut counts = BTreeMap::new();
        let mut published = Vec::new();
        let mut clock = decode_time(rocks.get(CLOCK)?)?;
        let mut bytes = 0usize;
        for input in inputs {
            let mut seen = BTreeSet::new();
            for row in input.rows {
                bytes = bytes
                    .saturating_add(row.key.len())
                    .saturating_add(row.value.as_ref().map_or(0, Vec::len));
                if row.key.len() > MAX_KEY_BYTES || bytes > MAX_COMMAND_BYTES {
                    return Err(Error::TooLarge);
                }
                if !seen.insert((row.table, &row.key)) {
                    return Err(Error::InvalidCommand("duplicate external row"));
                }
                if rocks.get(&table_key(row.table))?.is_none() {
                    return Err(Error::UnknownTable(row.table));
                }
                let key = data_key(row.table, &row.key);
                let identity = (row.table, row.key.clone());
                let (old_value, old_time, old_version) = match shadow.get(&identity) {
                    Some(prior) => prior.clone(),
                    None => (
                        rocks.get(&key)?,
                        decode_time(rocks.get(&time_key(row.table, &row.key))?)?,
                        optional_u64(
                            rocks.get(&version_key(row.table, &row.key))?,
                            "key generation",
                        )?,
                    ),
                };
                if old_version != 0 && row.time_term <= old_time {
                    if row.time_term == old_time && row.value != old_value {
                        return Err(Error::Corruption(
                            "equal logical time with different payload".into(),
                        ));
                    }
                    continue;
                }
                let generation = old_version.checked_add(1).ok_or(Error::VersionExhausted)?;
                let count = match counts.entry(row.table) {
                    std::collections::btree_map::Entry::Occupied(e) => e.into_mut(),
                    std::collections::btree_map::Entry::Vacant(e) => e.insert(required_u64(
                        rocks.get(&count_key(row.table))?,
                        "table count",
                    )?),
                };
                *count = count
                    .checked_add_signed(
                        i64::from(row.value.is_some()) - i64::from(old_value.is_some()),
                    )
                    .ok_or_else(|| Error::Corruption("table count overflow".into()))?;
                batch.push((key, row.value.clone()));
                batch.push((
                    time_key(row.table, &row.key),
                    Some(row.time_term.to_be_bytes().to_vec()),
                ));
                batch.push((
                    version_key(row.table, &row.key),
                    Some(generation.to_be_bytes().to_vec()),
                ));
                shadow.insert(identity, (row.value.clone(), row.time_term, generation));
                published.push((Key::Row(row.table, row.key.clone()), generation));
                clock = clock.max(row.time_term / 10);
            }
            let mut keys = BTreeSet::new();
            for (key, value) in input.metadata {
                bytes = bytes.saturating_add(key.len()).saturating_add(value.len());
                if bytes > MAX_COMMAND_BYTES {
                    return Err(Error::TooLarge);
                }
                if !keys.insert(key) {
                    return Err(Error::InvalidCommand("duplicate protocol metadata"));
                }
                let mut physical = METADATA.to_vec();
                physical.extend_from_slice(key);
                batch.push((physical, Some(value.clone())));
            }
        }
        let data_generation = if published.is_empty() {
            None
        } else {
            let generation = required_u64(rocks.get(GENERATION)?, "data generation")?
                .checked_add(1)
                .ok_or(Error::VersionExhausted)?;
            batch.push((GENERATION.to_vec(), Some(generation.to_be_bytes().to_vec())));
            batch.push((CLOCK.to_vec(), Some(clock.to_be_bytes().to_vec())));
            Some(generation)
        };
        for (table, count) in counts {
            batch.push((count_key(table), Some(count.to_be_bytes().to_vec())));
        }
        let ops: Vec<_> = batch
            .iter()
            .map(|(key, value)| match value {
                Some(val) => BlobOp::Put { key, val },
                None => BlobOp::Delete { key },
            })
            .collect();
        #[cfg(feature = "test-hooks")]
        let fault = self.apply_fault.swap(0, Ordering::AcqRel);
        #[cfg(feature = "test-hooks")]
        if fault == crate::ApplyFault::BeforeBatch as u8 {
            return Err(Error::Storage("injected before external batch".into()));
        }
        rocks.write_batch(&ops)?;
        #[cfg(feature = "test-hooks")]
        if fault == crate::ApplyFault::AfterBatch as u8 {
            return Err(Error::Storage("injected ambiguous external batch".into()));
        }
        let metadata = mutex(&self.metadata);
        for (key, generation) in published {
            if let Some(meta) = metadata.get(&key).and_then(Weak::upgrade) {
                meta.generation.store(generation, Ordering::Release);
            }
        }
        if let Some(generation) = data_generation {
            self.range.generation.store(generation, Ordering::Release);
        }
        Ok(())
    }
    /// Finish a private offline import after its rows were loaded in bounded
    /// batches. The directory must never have served traffic. Publication of the
    /// containing directory is the converter's responsibility. An interrupted
    /// import is discarded, never opened as a production database.
    pub fn finish_distributed_import(&self, history: &[Prefix], metadata: &Metadata) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_distributed()?;
        let result = (|| {
            let rocks = self.rocks();
            if read_position(&rocks)? != Position::default() || !read_metadata(&rocks)?.is_empty() {
                return Err(Error::InvalidState(
                    "import requires a private unpositioned database",
                ));
            }
            let mut batch = Vec::new();
            let mut previous = Position::default();
            for prefix in history {
                let p = prefix.position;
                if p.index != previous.index + 1 || p.term == 0 || p.term < previous.term {
                    return Err(Error::Corruption("invalid imported log prefix".into()));
                }
                let mut value = p.term.to_be_bytes().to_vec();
                value.extend_from_slice(&prefix.fingerprint);
                batch.push((entry_key(p.index), value));
                previous = p;
            }
            if previous.index == 0 {
                return Err(Error::InvalidState("empty imported log prefix"));
            }
            let mut keys = BTreeSet::new();
            for (key, value) in metadata {
                if !keys.insert(key) {
                    return Err(Error::InvalidCommand("duplicate imported metadata"));
                }
                let mut physical = METADATA.to_vec();
                physical.extend_from_slice(key);
                batch.push((physical, value.clone()));
            }
            batch.push((APPLIED.to_vec(), position_bytes(previous)));
            let ops = batch
                .iter()
                .map(|(key, val)| BlobOp::Put { key, val })
                .collect::<Vec<_>>();
            rocks.write_batch(&ops)?;
            Ok(())
        })();
        if result.is_err() {
            self.fence();
        }
        result
    }
    /// Logical page includes deleted keys. Caller pauses publication across pages.
    pub fn distributed_page(
        &self,
        table: u64,
        after: Option<&[u8]>,
        limit: usize,
    ) -> Result<(Vec<Row>, bool)> {
        if limit == 0 || limit > 4096 {
            return Err(Error::InvalidCommand("checkpoint page limit"));
        }
        let (snapshot, _, _) = self.snapshot()?;
        self.ensure_distributed()?;
        if snapshot.get(&table_key(table))?.is_none() {
            return Err(Error::UnknownTable(table));
        }
        let start = time_key(table, after.unwrap_or_default());
        let end = if table == u64::MAX {
            vec![0, 10]
        } else {
            time_key(table + 1, b"")
        };
        let mut rows = Vec::new();
        let mut end_of_table = true;
        let mut failure = None;
        snapshot.scan(&start, Some(&end), false, usize::MAX, &mut |key, value| {
            let key = &key[10..];
            if after.is_some_and(|after| key <= after) {
                return Ok(true);
            }
            if rows.len() == limit {
                end_of_table = false;
                return Ok(false);
            }
            let time_term = match decode_time(Some(value.to_vec())) {
                Ok(t) => t,
                Err(e) => {
                    failure = Some(e);
                    return Ok(false);
                }
            };
            rows.push(Row {
                table,
                key: key.to_vec(),
                value: snapshot.get(&data_key(table, key))?,
                time_term,
            });
            Ok(true)
        })?;
        if let Some(error) = failure {
            return Err(error);
        }
        Ok((rows, end_of_table))
    }
}

pub(crate) fn read_metadata(rocks: &mako_rocksdb_store::RocksBlobs) -> Result<Metadata> {
    let mut rows = Vec::new();
    rocks.for_each_entry_from(METADATA, usize::MAX, &mut |key, value| {
        if !key.starts_with(METADATA) {
            return Ok(false);
        }
        rows.push((key[2..].to_vec(), value.to_vec()));
        Ok(true)
    })?;
    Ok(rows)
}
