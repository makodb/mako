//! The value record and the per-key entry.
//!
//! These two types encode most of the cache's correctness. Read the notes
//! before changing them — several of the shapes here look like they could
//! be simplified, and each simplification is a bug the C++ original had
//! to be taught the hard way.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use crate::record::Record;

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
    /// A `Mutex<Arc<Val>>` rather than an atomic pointer or `ArcSwap`:
    /// this crate is `forbid(unsafe_code)` and depends on nothing outside
    /// std, which keeps the eventual CMake integration free of a vendored
    /// dependency tree. Publication is still expressed as an explicit
    /// compare-and-swap against the *exact* record read (see
    /// [`Entry::compare_publish`]) so the semantics — and therefore the
    /// mutation coverage — match the lock-free original. Swapping in a
    /// lock-free slot later is a change behind these same tests.
    val: Mutex<Record>,
    /// CLOCK second-chance bit. Set on access, cleared by the sweeper.
    referenced: AtomicBool,
}

impl Entry {
    /// Create an entry holding `initial`.
    pub fn new(key: &[u8], initial: Record) -> Self {
        Self {
            key: key.to_vec().into_boxed_slice(),
            val: Mutex::new(initial),
            referenced: AtomicBool::new(true),
        }
    }

    /// This entry's key.
    pub fn key(&self) -> &[u8] {
        &self.key
    }

    /// The currently published value.
    pub fn load(&self) -> Record {
        self.val.lock().expect("entry mutex poisoned").clone()
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
    /// still, `memcpy` the bytes into the caller's buffer, let go. It
    /// holds an RCU epoch where this holds the slot lock, but neither
    /// hands a reference to the value outward, which is what makes the
    /// copy the only thing the caller keeps.
    ///
    /// `f` must not block: it runs with the slot held, and every reader
    /// and writer of this key is behind it.
    pub fn with_value<R>(&self, f: impl FnOnce(&Record) -> R) -> R {
        self.referenced.store(true, Ordering::Relaxed);
        let slot = self.val.lock().expect("entry mutex poisoned");
        f(&slot)
    }

    /// Publish `new` only if the currently published record is still
    /// *exactly* `expected`.
    ///
    /// Identity, not equality: two records can be equal in content and
    /// distinct in obligation. Returns `true` if the swap happened.
    pub fn compare_publish(&self, expected: &Record, new: Record) -> bool {
        let displaced = {
            let mut slot = self.val.lock().expect("entry mutex poisoned");
            if !Record::ptr_eq(&slot, expected) {
                return false;
            }
            std::mem::replace(&mut *slot, new)
        };
        // Freed with the lock released, as in `with_slot`.
        drop(displaced);
        true
    }

    /// Publish unconditionally, returning the displaced record.
    ///
    /// Used only where the caller has already decided under the same
    /// critical section (the write path), never to blind-overwrite a
    /// value another thread may have published.
    pub fn publish(&self, new: Record) -> Record {
        let mut slot = self.val.lock().expect("entry mutex poisoned");
        std::mem::replace(&mut *slot, new)
    }

    /// Run `f` against the published record while holding the slot, and
    /// publish whatever it returns.
    ///
    /// This is how the write path stays atomic per key without a separate
    /// retry loop: the decision (does the key exist? may this write
    /// proceed?) and the publication happen under one acquisition, so no
    /// other writer can interleave between them.
    pub fn with_slot<R>(&self, f: impl FnOnce(&Record) -> (Option<Record>, R)) -> R {
        let (displaced, r) = {
            let mut slot = self.val.lock().expect("entry mutex poisoned");
            let (new, r) = f(&slot);
            (new.map(|v| std::mem::replace(&mut *slot, v)), r)
        };
        // The lock is RELEASED here, and only then does the displaced
        // record's refcount reach zero and its allocation get freed.
        //
        // `*slot = v` dropped it inside the critical section, so every
        // overwrite ran `free()` — of a block another thread almost
        // certainly allocated — while holding the lock that every reader
        // and writer of this key is queued behind. The C++ implementation
        // never frees on this path at all: it pushes the old record onto a
        // thread-local queue and defers the free to an epoch boundary.
        // This does not defer, but it does get the allocator out of the
        // critical section.
        drop(displaced);
        r
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
