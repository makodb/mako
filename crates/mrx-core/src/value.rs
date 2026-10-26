//! The value record and the per-key entry.
//!
//! These two types encode most of the cache's correctness. Read the notes
//! before changing them — several of the shapes here look like they could
//! be simplified, and each simplification is a bug the C++ original had
//! to be taught the hard way.

use std::sync::atomic::{AtomicBool, Ordering};

use crate::record::{Kind, Record};
use crate::slot::Slot;

// `Val` and `ValState` used to live here: an `Arc<Val>` whose
// `ValState::Resident` held a `Vec<u8>`. That was TWO allocations per
// write and two frees per overwrite. They are replaced by
// [`crate::Record`], one allocation with the payload inline, which is
// the shape the C++ `mrx_val` has always had.

/// One key's entry: stable for the life of the store.
///
/// Entries are **immortal**. Nothing ever removes one, including
/// `truncate`. That is load-bearing rather than lazy: the flusher's dirty
/// map holds entry references across cycles, and a `truncate` that freed
/// them would have to stop the flusher first — which the C++ original
/// deliberately does not do.
#[derive(Debug)]
pub struct Entry {
    key: Box<[u8]>,
    /// The current value.
    ///
    /// An atomic pointer replaced by compare-and-swap, with reclamation
    /// deferred through an epoch — see [`crate::slot`] for the protocol
    /// and for why a `Mutex<Record>` (which this replaced) charged the
    /// read path a store to a shared word, a clone and a drop.
    val: Slot,
    /// CLOCK second-chance bit. Set on access, cleared by the sweeper.
    referenced: AtomicBool,
}

impl Entry {
    /// Create an entry holding `initial`.
    pub fn new(key: &[u8], initial: Record) -> Self {
        Self {
            key: key.to_vec().into_boxed_slice(),
            val: Slot::new(initial),
            referenced: AtomicBool::new(true),
        }
    }

    /// This entry's key.
    pub fn key(&self) -> &[u8] {
        &self.key
    }

    /// The currently published value.
    pub fn load(&self) -> Record {
        self.val.load()
    }

    /// Load and mark the entry as recently used.
    pub fn load_touched(&self) -> Record {
        self.referenced.store(true, Ordering::Relaxed);
        self.load()
    }

    /// Run `f` against the published record **without cloning the `Arc`**.
    ///
    /// The difference from [`Entry::load`] is one atomic increment and one
    /// atomic decrement on the record's refcount — and on a hot key those
    /// land on the same cache line for every reader, so they contend
    /// exactly where reads are heaviest. Measured on a 2000-key hot set
    /// with 16 threads, `load` + copy costs 31 ns against a 15 ns masstree
    /// lookup; the refcount is roughly half of it.
    ///
    /// This is the shape the C++ implementation uses: hold the record
    /// still, copy the bytes into the caller's buffer, let go. It holds
    /// an epoch guard, exactly as the C++ read side holds an RCU region,
    /// and neither hands a reference to the value outward — which is what
    /// makes the copy the only thing the caller keeps.
    ///
    /// Nothing is locked, so `f` may block; doing so only delays the
    /// reclamation of records another thread has already displaced.
    pub fn with_value<R>(&self, f: impl FnOnce(&Record) -> R) -> R {
        self.referenced.store(true, Ordering::Relaxed);
        self.val.with_value(f)
    }

    /// Copy the published bytes into `out`, reusing its capacity.
    ///
    /// `None` means the value is not resident and a fill is needed;
    /// `Some(false)` means the key is deleted, and `out` is left empty.
    /// This is the read path's shape: no handle is created, so no
    /// refcount is touched, and no `Vec` is allocated per read.
    pub fn copy_into(&self, out: &mut Vec<u8>) -> Option<bool> {
        self.referenced.store(true, Ordering::Relaxed);
        self.val.with_value(|v| match v.kind() {
            Kind::Tombstone => Some(false),
            Kind::Resident => {
                out.extend_from_slice(v.bytes().expect("a resident record has bytes"));
                Some(true)
            }
            Kind::Evicted => None,
        })
    }

    /// Copy the published bytes into a fixed slice.
    ///
    /// `None` means the value is not resident (a fill is needed),
    /// `Some(Absent)` means deleted, and `Some(TooSmall(len))` reports a
    /// value that does not fit without copying it.
    pub fn copy_into_slice(&self, out: &mut [u8]) -> Option<crate::CopyOutcome> {
        self.referenced.store(true, Ordering::Relaxed);
        self.val.with_value(|v| match v.kind() {
            Kind::Tombstone => Some(crate::CopyOutcome::Absent),
            Kind::Resident => {
                let bytes = v.bytes().expect("a resident record has bytes");
                Some(crate::place(bytes, out))
            }
            Kind::Evicted => None,
        })
    }

    /// Publish `new` only if the currently published record is still
    /// *exactly* `expected`.
    ///
    /// Identity, not equality: two records can be equal in content and
    /// distinct in obligation. Returns `true` if the swap happened.
    pub fn compare_publish(&self, expected: &Record, new: Record) -> bool {
        self.val.compare_publish(expected, new)
    }

    /// Run `f` against the published record, publish whatever it returns,
    /// and retry `f` if another writer wins in between.
    ///
    /// This is how the write path stays atomic per key: the decision (does
    /// the key exist? may this write proceed?) is made against the exact
    /// record the compare-and-swap then compares, so no other writer can
    /// interleave between the decision and the publication.
    ///
    /// `f` must therefore be re-runnable: on contention its `R` is
    /// discarded and it is called again with the record that won. Nothing
    /// derived from the first attempt is observable — no publication
    /// happened — but side effects inside `f` do repeat (the write path
    /// draws a fresh version and allocates a fresh record per attempt,
    /// exactly as the C++ `mrx_write` retry loop does).
    pub fn with_slot<R>(&self, f: impl FnMut(&Record) -> (Option<Record>, R)) -> R {
        self.val.with_slot(f)
    }

    /// Take the CLOCK bit, clearing it. `true` means "recently used, give
    /// it a second chance".
    pub fn take_referenced(&self) -> bool {
        self.referenced.swap(false, Ordering::AcqRel)
    }

    /// Mark as recently used.
    pub fn touch(&self) {
        self.referenced.store(true, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compare_publish_is_identity_not_equality() {
        let a = Record::resident(1, b"same");
        let twin = Record::resident(1, b"same"); // equal, not identical
        let e = Entry::new(b"k", a.clone());

        assert!(
            !e.compare_publish(&twin, Record::resident(2, b"new")),
            "an equal-but-distinct record must NOT satisfy the comparand"
        );
        assert!(e.compare_publish(&a, Record::resident(2, b"new")));
        assert_eq!(e.load().version(), 2);
    }

    #[test]
    fn with_value_sees_the_published_record_and_marks_it_used() {
        let e = Entry::new(b"k", Record::resident(7, b"hello"));
        let (ver, bytes) = e.with_value(|v| (v.version(), v.bytes().map(<[u8]>::to_vec)));
        assert_eq!(ver, 7);
        assert_eq!(bytes.as_deref(), Some(&b"hello"[..]));
        assert!(e.take_referenced(), "a read marks the entry recently used");
    }

    #[test]
    fn clock_bit_is_taken_once() {
        let e = Entry::new(b"k", Record::resident(1, &[]));
        assert!(e.take_referenced(), "starts set");
        assert!(!e.take_referenced(), "and taking it clears it");
        e.touch();
        assert!(e.take_referenced());
    }
}
