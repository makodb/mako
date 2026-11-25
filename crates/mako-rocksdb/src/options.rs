use crate::{Error, Result};

/// Supported opening options. Other RocksDB tuning methods are intentionally absent.
#[derive(Clone, Debug)]
pub struct Options {
    pub(crate) create: bool,
    pub(crate) error_if_exists: bool,
    pub(crate) masstree: bool,
    pub(crate) max_transactions: u32,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            create: false,
            error_if_exists: false,
            masstree: false,
            max_transactions: 4,
        }
    }
}
impl Options {
    pub fn create_if_missing(&mut self, value: bool) {
        self.create = value;
    }
    pub fn set_error_if_exists(&mut self, value: bool) {
        self.error_if_exists = value;
    }
    /// Mako extension. Reopen the same durable image with or without Masstree.
    pub fn set_masstree(&mut self, value: bool) {
        self.masstree = value;
    }
    /// Mako extension. Concurrent transactions consume separate coordinator streams.
    pub fn set_max_transactions(&mut self, value: u32) {
        self.max_transactions = value;
    }
}

#[derive(Clone, Debug, Default)]
pub struct WriteOptions {
    pub(crate) sync: bool,
    pub(crate) disable_wal: bool,
}
impl WriteOptions {
    pub fn set_sync(&mut self, value: bool) {
        self.sync = value;
    }
    pub fn disable_wal(&mut self, value: bool) {
        self.disable_wal = value;
    }
    pub(crate) fn validate(&self) -> Result<()> {
        if self.disable_wal {
            return Err(Error::unsupported(
                "Mako transactions require the decision log",
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default)]
pub struct OptimisticTransactionOptions {
    snapshot: bool,
}
impl OptimisticTransactionOptions {
    pub fn set_snapshot(&mut self, value: bool) {
        self.snapshot = value;
    }
    pub(crate) fn validate(&self) -> Result<()> {
        if self.snapshot {
            return Err(Error::unsupported(
                "MVCC snapshots are not supported; Mako certifies reads at prepare",
            ));
        }
        Ok(())
    }
}

/// Bytewise range bounds. Iterators include lower and exclude upper.
#[derive(Clone, Debug, Default)]
pub struct ReadOptions {
    pub(crate) lower: Option<Vec<u8>>,
    pub(crate) upper: Option<Vec<u8>>,
}
impl ReadOptions {
    pub fn set_iterate_lower_bound<K: Into<Vec<u8>>>(&mut self, key: K) {
        self.lower = Some(key.into());
    }
    pub fn set_iterate_upper_bound<K: Into<Vec<u8>>>(&mut self, key: K) {
        self.upper = Some(key.into());
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Direction {
    Forward,
    Reverse,
}
#[derive(Clone, Copy, Debug)]
pub enum IteratorMode<'a> {
    Start,
    End,
    From(&'a [u8], Direction),
}
