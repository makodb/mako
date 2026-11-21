//! Worker-owned storage for the wrapper's canonical operation log.
//!
//! Only the active prefix is part of the current transaction. Recycling
//! retains small key/value buffers, with a fixed per-worker memory ceiling.

use std::ops::{Deref, DerefMut};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Staged {
    Untouched,
    Live,
    Deleted,
}

pub(crate) struct Touched {
    pub(crate) table_id: u64,
    pub(crate) key: Vec<u8>,
    pub(crate) existed_before: Option<bool>,
    pub(crate) staged: Staged,
    pub(crate) value: Vec<u8>,
    pub(crate) read: bool,
    pub(crate) wrote: bool,
    pub(crate) inserted: bool,
    pub(crate) deleted: bool,
}

/// Move the pooled ownership handle, not the log's vectors and lease state.
/// An empty transaction never allocates storage.
#[derive(Default)]
pub(crate) struct TouchLog {
    state: Option<Box<TouchLogState>>,
}

impl TouchLog {
    #[inline]
    pub(crate) fn record_cleanup(&mut self, complete: bool) {
        if let Some(state) = self.state.as_mut() {
            state.record_cleanup(complete);
        }
    }

    pub(crate) fn recycle(&mut self) {
        if let Some(state) = self.state.as_mut() {
            state.recycle();
        }
    }
}

impl Deref for TouchLog {
    type Target = TouchLogState;

    #[inline]
    fn deref(&self) -> &Self::Target {
        self.state.as_deref().unwrap_or(&EMPTY_TOUCH_LOG)
    }
}

impl DerefMut for TouchLog {
    #[inline]
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.state.get_or_insert_with(Box::default)
    }
}

static EMPTY_TOUCH_LOG: TouchLogState = TouchLogState {
    entries: Vec::new(),
    used: 0,
    retired: Vec::new(),
    borrowed: Borrowed::Never,
    cleanup_complete: false,
};

#[derive(Default)]
pub(crate) struct TouchLogState {
    entries: Vec<Touched>,
    used: usize,
    // Only the first operation can lend a payload, and it always occupies
    // entry zero. Later operations use ordinary owned core values. This one
    // retirement slot keeps the first allocation immutable after overwrite.
    retired: Vec<u8>,
    borrowed: Borrowed,
    cleanup_complete: bool,
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum Borrowed {
    #[default]
    Never,
    Prepared,
    Live,
    Retired,
}

impl TouchLogState {
    #[inline]
    pub(crate) fn can_borrow_first(&self) -> bool {
        self.used == 0 && self.borrowed == Borrowed::Never
    }

    /// Anchor the only borrowed payload before entering any core callback.
    /// The prepared entry is not canonical until the put succeeds.
    #[inline]
    pub(crate) fn prepare_first(&mut self, table_id: u64, key: &[u8], value: &[u8]) -> &[u8] {
        assert!(self.can_borrow_first());
        let index = self.touch(table_id, key);
        assert_eq!(index, 0);
        self.used = 0;
        // Do not retain a large prior allocation for this bounded lane.
        if self.entries[0].value.capacity() > sto_masstree::STABLE_ATOMIC_VALUE_CAPACITY {
            self.entries[0].value = Vec::new();
        }
        self.entries[0].value.extend_from_slice(value);
        self.cleanup_complete = false;
        self.borrowed = Borrowed::Prepared;
        &self.entries[0].value
    }

    #[inline]
    pub(crate) fn accept_first(&mut self) -> usize {
        assert!(self.borrowed == Borrowed::Prepared && self.used == 0);
        self.used = 1;
        self.borrowed = Borrowed::Live;
        0
    }

    /// Header moves preserve the borrowed allocation's address and bytes.
    pub(crate) fn retire_value(&mut self, index: usize) {
        if index == 0 && matches!(self.borrowed, Borrowed::Prepared | Borrowed::Live) {
            debug_assert!(self.retired.is_empty());
            self.retired = std::mem::take(&mut self.entries[0].value);
            self.borrowed = Borrowed::Retired;
        }
    }

    pub(crate) fn record_cleanup(&mut self, complete: bool) {
        self.cleanup_complete = complete;
    }

    #[inline]
    pub(crate) fn value_is_borrowed(&self, index: usize) -> bool {
        index == 0 && !self.cleanup_complete && self.borrowed == Borrowed::Live
    }

    // Returning the owner makes the quarantine boundary testable without
    // leaking test fixtures. Production callers deliberately retain it.
    #[inline]
    fn take_unreleased_payload(&mut self) -> Option<Vec<u8>> {
        if self.cleanup_complete {
            return None;
        }
        let value = match self.borrowed {
            Borrowed::Never => return None,
            Borrowed::Prepared | Borrowed::Live => std::mem::take(&mut self.entries[0].value),
            Borrowed::Retired => std::mem::take(&mut self.retired),
        };
        self.borrowed = Borrowed::Never;
        Some(value)
    }

    #[inline]
    fn pin_unreleased_payload(&mut self) {
        if let Some(value) = self.take_unreleased_payload() {
            // Core quarantine can retain an intent forever. The holder pool
            // is caller-owned and is not an adequate owner for this borrow.
            std::mem::forget(value);
        }
    }

    pub(crate) fn has_storage(&self) -> bool {
        self.entries.capacity() != 0
    }

    pub(crate) fn touch(&mut self, table_id: u64, key: &[u8]) -> usize {
        // Fast path: the one-put steady state reuses slot zero with no scan.
        // `put` invalidates the witness on any second operation, so a used
        // prefix of length one with a live first entry cannot alias this key
        // unless it is the same key; the equality check below proves that.
        if self.used == 1 && self.entries.len() == 1 {
            let entry = &mut self.entries[0];
            if entry.table_id == table_id && entry.key == key {
                return 0;
            }
        } else if let Some(index) = self
            .iter()
            .position(|entry| entry.table_id == table_id && entry.key == key)
        {
            return index;
        }
        let index = self.used;
        self.retire_value(index);
        if index == self.entries.len() {
            self.entries.push(Touched {
                table_id: 0,
                key: Vec::new(),
                existed_before: None,
                staged: Staged::Untouched,
                value: Vec::new(),
                read: false,
                wrote: false,
                inserted: false,
                deleted: false,
            });
        }
        let entry = &mut self.entries[index];
        entry.table_id = table_id;
        entry.key.clear();
        entry.key.extend_from_slice(key);
        entry.value.clear();
        entry.existed_before = None;
        entry.staged = Staged::Untouched;
        entry.read = false;
        entry.wrote = false;
        entry.inserted = false;
        entry.deleted = false;
        self.used += 1;
        index
    }

    pub(crate) fn recycle(&mut self) {
        // Fast path: the one-put steady state keeps exactly one entry with
        // no outstanding borrow. Clearing it in place skips the bounded
        // 64-entry/64 KiB retention scan below; backing storage is retained.
        // `cleanup_complete` proves the unreleased-payload pin below is a
        // no-op, so skipping it is equivalent.
        if self.used == 1 && self.cleanup_complete && self.entries.len() == 1 {
            let entry = &mut self.entries[0];
            entry.key.clear();
            entry.value.clear();
            entry.existed_before = None;
            entry.staged = Staged::Untouched;
            entry.read = false;
            entry.wrote = false;
            entry.inserted = false;
            entry.deleted = false;
            self.retired.clear();
            self.borrowed = Borrowed::Never;
            self.cleanup_complete = false;
            self.used = 0;
            return;
        }
        self.pin_unreleased_payload();
        self.borrowed = Borrowed::Never;
        self.cleanup_complete = false;
        // A clean multi-item attempt must also retire its first-value lease;
        // otherwise all later attempts permanently miss bounded staging.
        self.retired = Vec::new();
        // Keep at most 64 entries and 64 KiB of backing allocations after a
        // large transaction. The one-put case retains its existing buffers.
        const MAX_ENTRIES: usize = 64;
        const MAX_BYTES: usize = 64 * 1024;
        self.used = 0;
        self.entries.truncate(MAX_ENTRIES);
        if self.entries.capacity() > MAX_ENTRIES {
            self.entries.shrink_to(MAX_ENTRIES);
        }
        let mut remaining =
            MAX_BYTES.saturating_sub(self.entries.capacity() * std::mem::size_of::<Touched>());
        for entry in &mut self.entries {
            for buffer in [&mut entry.key, &mut entry.value] {
                buffer.clear();
                if buffer.capacity() > remaining {
                    *buffer = Vec::new();
                } else {
                    remaining -= buffer.capacity();
                }
            }
        }
    }
}

impl Drop for TouchLogState {
    #[inline]
    fn drop(&mut self) {
        // Also covers unwinding before a wrapper terminal obtains a report.
        self.pin_unreleased_payload();
    }
}

impl Deref for TouchLogState {
    type Target = [Touched];
    fn deref(&self) -> &Self::Target {
        &self.entries[..self.used]
    }
}

impl DerefMut for TouchLogState {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.entries[..self.used]
    }
}

impl<'a> IntoIterator for &'a TouchLog {
    type Item = &'a Touched;
    type IntoIter = std::slice::Iter<'a, Touched>;
    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recycled_log_forgets_effects_and_preserves_small_storage() {
        let mut log = TouchLog::default();
        let index = log.touch(7, b"first");
        log[index].value.extend_from_slice(b"payload");
        log[index].existed_before = Some(true);
        log[index].staged = Staged::Deleted;
        log[index].read = true;
        log[index].wrote = true;
        log[index].inserted = true;
        log[index].deleted = true;
        let key_storage = log[index].key.as_ptr();
        let value_storage = log[index].value.as_ptr();
        log.recycle();
        assert!(log.is_empty());
        let index = log.touch(9, b"next");
        assert_eq!(index, 0);
        assert_eq!(log[index].table_id, 9);
        assert_eq!(log[index].key, b"next");
        assert_eq!(log[index].key.as_ptr(), key_storage);
        assert_eq!(log[index].value.as_ptr(), value_storage);
        assert!(log[index].value.is_empty());
        assert!(log[index].existed_before.is_none());
        assert!(matches!(log[index].staged, Staged::Untouched));
        assert!(
            !log[index].read && !log[index].wrote && !log[index].inserted && !log[index].deleted
        );
    }

    #[test]
    fn recycling_caps_retained_transaction_memory() {
        let mut log = TouchLog::default();
        for id in 0..200u64 {
            let index = log.touch(1, &id.to_be_bytes());
            log[index].value.resize(4096, 0);
        }
        log.recycle();
        let retained = log.entries.capacity() * std::mem::size_of::<Touched>()
            + log
                .entries
                .iter()
                .map(|entry| entry.key.capacity() + entry.value.capacity())
                .sum::<usize>();
        assert!(log.is_empty());
        assert!(log.entries.len() <= 64);
        assert!(retained <= 64 * 1024);
    }
}

#[cfg(test)]
#[path = "touched_borrow_tests.rs"]
mod borrow_tests;
