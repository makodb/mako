//! A pull-based iterator over the cache.

use std::collections::VecDeque;

use crate::{Error, MrxStore};

/// Which way an [`Iter`] walks.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    /// Ascending.
    Forward,
    /// Descending.
    Reverse,
}

/// A chunked iterator over live key/value pairs.
///
/// Yields `Result` because a value may have been evicted, and fetching it
/// back can fail. Silently skipping such a key would turn a transient IO
/// error into apparent data loss, so the error is surfaced.
///
/// The iterator holds no lock and pins nothing: keys written after it
/// starts may or may not appear, and a key deleted mid-walk may still be
/// yielded if it was already buffered. It is a chunked scan, not a
/// snapshot — the cache has no MVCC to build one from.
pub struct Iter<'a> {
    store: &'a MrxStore,
    dir: Direction,
    buf: VecDeque<(Vec<u8>, Vec<u8>)>,
    cursor: Option<Vec<u8>>,
    skip_first: bool,
    done: bool,
}

impl<'a> Iter<'a> {
    pub(crate) fn new(store: &'a MrxStore, from: &[u8], dir: Direction) -> Self {
        Self {
            store,
            dir,
            buf: VecDeque::new(),
            cursor: Some(from.to_vec()),
            skip_first: false,
            done: false,
        }
    }

    fn refill(&mut self) -> Result<(), Error> {
        while self.buf.is_empty() && !self.done {
            let Some(from) = self.cursor.take() else {
                self.done = true;
                return Ok(());
            };
            let chunk = self
                .store
                .chunk(&from, self.dir == Direction::Forward, self.skip_first)?;
            self.buf.extend(chunk.pairs);
            match chunk.next_from {
                // A chunk can be empty while the range continues — every
                // key in it was deleted — so this loops rather than
                // returning, or an all-tombstone chunk would end the walk
                // early.
                Some(next) => {
                    self.cursor = Some(next);
                    self.skip_first = true;
                }
                None => self.done = true,
            }
        }
        Ok(())
    }
}

impl Iterator for Iter<'_> {
    type Item = Result<(Vec<u8>, Vec<u8>), Error>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Err(e) = self.refill() {
            self.done = true;
            return Some(Err(e));
        }
        self.buf.pop_front().map(Ok)
    }
}
