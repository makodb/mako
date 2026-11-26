//! RocksDB log/base through the maintained Rust crate.
//! Journal batches use RocksDB's WAL barrier. Segmented-WAL bases with synchronous
//! persistence disable the redundant RocksDB WAL and wait for a memtable flush;
//! async bases retain it for process-crash recovery after segment reclamation.
#![forbid(unsafe_code)]

use rocksdb::{DBCompressionType, FlushOptions, Options, WriteBatch, WriteOptions, DB};
use std::{cell::Cell, io, marker::PhantomData, path::Path};
use crate::base::{Base, Op};

/// A RocksDB base. Not `Sync`; one owner (the applier, or recovery).
pub struct RocksBase {
    db: DB,
    wopts: WriteOptions,
    sync_writes: bool,
    journal: bool,
    _not_sync: PhantomData<Cell<()>>,
}

impl RocksBase {
    /// Create a new base, or reopen an existing one without replacing missing data.
    pub fn open(path: &Path, create: bool) -> io::Result<Self> {
        Self::open_with_sync_writes(path, create, true)
    }

    pub fn open_with_sync_writes(path: &Path, create: bool, sync_writes: bool) -> io::Result<Self> {
        Self::open_policy(path, create, sync_writes, false)
    }

    /// Shared local/Raft log: RocksDB WAL owns the persistence barrier.
    pub fn open_journal(path: &Path, create: bool, sync_writes: bool) -> io::Result<Self> {
        Self::open_policy(path, create, sync_writes, true)
    }

    fn open_policy(path: &Path, create: bool, sync_writes: bool, journal: bool) -> io::Result<Self> {
        let mut options = Options::default();
        options.create_if_missing(create);
        options.set_error_if_exists(create);
        // Preserve the ingest policy: 64 MiB buffers, four buffers, no compression.
        options.set_write_buffer_size(64 << 20);
        options.set_max_write_buffer_number(4);
        options.set_compression_type(DBCompressionType::None);
        let db = DB::open(&options, path).map_err(io::Error::other)?;
        let mut wopts = WriteOptions::default();
        wopts.disable_wal(sync_writes && !journal);
        wopts.set_sync(sync_writes && journal);
        Ok(Self { db, wopts, sync_writes, journal, _not_sync: PhantomData })
    }
}

impl Base for RocksBase {
    fn persist(&mut self, ops: &[Op]) -> io::Result<()> {
        self.write(ops)?;
        if self.journal { Ok(()) } else { self.flush() }
    }

    fn write(&mut self, ops: &[Op]) -> io::Result<()> {
        let mut batch = WriteBatch::default();
        for op in ops {
            match op {
                Op::Put(key, value) => batch.put(key, value),
                Op::DeleteRange(start, end) => batch.delete_range(start, end),
            }
        }
        self.db.write_opt(batch, &self.wopts).map_err(io::Error::other)
    }

    fn flush(&mut self) -> io::Result<()> {
        let mut options = FlushOptions::default();
        options.set_wait(self.sync_writes || self.journal);
        self.db.flush_opt(&options).map_err(io::Error::other)
    }

    fn get(&self, key: &[u8]) -> io::Result<Option<Vec<u8>>> {
        self.db.get(key).map_err(io::Error::other)
    }

    fn scan(&self, prefix: &[u8]) -> io::Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let mut out = Vec::new();
        let mut iterator = self.db.raw_iterator();
        iterator.seek(prefix);
        while let Some((key, value)) = iterator.item() {
            if !key.starts_with(prefix) { break; }
            out.push((key.to_vec(), value.to_vec()));
            iterator.next();
        }
        iterator.status().map_err(io::Error::other)?;
        Ok(out)
    }
}
