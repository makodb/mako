//! The value record and the per-key entry.
//!
//! These two types encode most of the cache's correctness. Read the notes
//! before changing them — several of the shapes here look like they could
//! be simplified, and each simplification is a bug the C++ original had
//! to be taught the hard way.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::Version;

/// What a key's value currently is.
///
/// Three explicit states, deliberately not two-plus-an-`Option`.
///
/// `Evicted` **carries its version**. Representing it as `None` — the
/// obvious Rust move — is exactly the null-comparison the ABA guard
/// exists to prevent: a fill reads "absent", goes to the durable store,
/// and installs while a writer has published, flushed, and re-evicted in
/// the meantime. The version is what makes that detectable.
///
/// `Evicted` also means "already durable", because eviction requires
/// `version <= W`. Writeback uses that: an evicted entry needs no write.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ValState {
    /// Bytes are in memory.
    Resident(Vec<u8>),
    /// Bytes live only in the durable store. Version retained.
    Evicted,
    /// The key is deleted. Retained forever — see trap 1.
    Tombstone,
}

/// An immutable value record.
///
/// Immutable *once published* is the whole point: nothing mutates a
/// published `Val`, so a reader holding one can never observe a torn
/// state, and the flusher can compare versions to decide whether what it
/// read is still current.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Val {
    /// The version stamped when this record was published.
    pub version: Version,
    /// What the value is.
    pub state: ValState,
}

impl Val {
    /// A live value.
    pub fn resident(version: Version, bytes: Vec<u8>) -> Arc<Self> {
        Arc::new(Self {
            version,
            state: ValState::Resident(bytes),
        })
    }

    /// A deleted key.
    pub fn tombstone(version: Version) -> Arc<Self> {
        Arc::new(Self {
            version,
            state: ValState::Tombstone,
        })
    }

    /// An evicted value, carrying forward the version whose bytes are in
    /// the durable store.
    pub fn evicted(version: Version) -> Arc<Self> {
        Arc::new(Self {
            version,
            state: ValState::Evicted,
        })
    }

    /// Whether this key currently exists. An *evicted* value still
    /// exists — only a tombstone means absent.
    pub fn is_live(&self) -> bool {
        !matches!(self.state, ValState::Tombstone)
    }

    /// Bytes, if they are in memory.
    pub fn bytes(&self) -> Option<&[u8]> {
        match &self.state {
            ValState::Resident(b) => Some(b),
            _ => None,
        }
    }

    /// Bytes this record occupies. Only resident values can be
    /// reclaimed, which is why the capacity check subtracts a floor.
    pub fn payload_bytes(&self) -> u64 {
        match &self.state {
            ValState::Resident(b) => b.len() as u64,
            _ => 0,
        }
    }
}

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
    val: Mutex<Arc<Val>>,
    /// CLOCK second-chance bit. Set on access, cleared by the sweeper.
    referenced: AtomicBool,
}

impl Entry {
    /// Create an entry holding `initial`.
    pub fn new(key: &[u8], initial: Arc<Val>) -> Self {
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
    pub fn load(&self) -> Arc<Val> {
        Arc::clone(&self.val.lock().expect("entry mutex poisoned"))
    }

    /// Load and mark the entry as recently used.
    pub fn load_touched(&self) -> Arc<Val> {
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
    pub fn with_value<R>(&self, f: impl FnOnce(&Val) -> R) -> R {
        self.referenced.store(true, Ordering::Relaxed);
        let slot = self.val.lock().expect("entry mutex poisoned");
        f(&slot)
    }

    /// Publish `new` only if the currently published record is still
    /// *exactly* `expected`.
    ///
    /// Identity, not equality: two records can be equal in content and
    /// distinct in obligation. Returns `true` if the swap happened.
    pub fn compare_publish(&self, expected: &Arc<Val>, new: Arc<Val>) -> bool {
        let mut slot = self.val.lock().expect("entry mutex poisoned");
        if !Arc::ptr_eq(&slot, expected) {
            return false;
        }
        *slot = new;
        true
    }

    /// Publish unconditionally, returning the displaced record.
    ///
    /// Used only where the caller has already decided under the same
    /// critical section (the write path), never to blind-overwrite a
    /// value another thread may have published.
    pub fn publish(&self, new: Arc<Val>) -> Arc<Val> {
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
    pub fn with_slot<R>(&self, f: impl FnOnce(&Arc<Val>) -> (Option<Arc<Val>>, R)) -> R {
        let mut slot = self.val.lock().expect("entry mutex poisoned");
        let (new, r) = f(&slot);
        if let Some(v) = new {
            *slot = v;
        }
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
    fn evicted_retains_its_version() {
        // If this ever becomes a versionless "None", the ABA guard in the
        // fill path silently stops working.
        let v = Val::evicted(42);
        assert_eq!(v.version, 42);
        assert!(v.is_live(), "an evicted value still EXISTS");
        assert_eq!(v.bytes(), None);
    }

    #[test]
    fn only_a_tombstone_means_absent() {
        assert!(Val::resident(1, b"x".to_vec()).is_live());
        assert!(Val::evicted(1).is_live());
        assert!(!Val::tombstone(1).is_live());
    }

    #[test]
    fn only_resident_values_have_reclaimable_bytes() {
        assert_eq!(Val::resident(1, vec![0u8; 100]).payload_bytes(), 100);
        assert_eq!(Val::evicted(1).payload_bytes(), 0);
        assert_eq!(Val::tombstone(1).payload_bytes(), 0);
    }

    #[test]
    fn compare_publish_is_identity_not_equality() {
        let a = Val::resident(1, b"same".to_vec());
        let twin = Val::resident(1, b"same".to_vec()); // equal, not identical
        let e = Entry::new(b"k", Arc::clone(&a));

        assert!(
            !e.compare_publish(&twin, Val::resident(2, b"new".to_vec())),
            "an equal-but-distinct record must NOT satisfy the comparand"
        );
        assert!(e.compare_publish(&a, Val::resident(2, b"new".to_vec())));
        assert_eq!(e.load().version, 2);
    }

    #[test]
    fn with_value_sees_the_published_record_and_marks_it_used() {
        let e = Entry::new(b"k", Val::resident(7, b"hello".to_vec()));
        let (ver, bytes) =
            e.with_value(|v| (v.version, v.bytes().map(<[u8]>::to_vec)));
        assert_eq!(ver, 7);
        assert_eq!(bytes.as_deref(), Some(&b"hello"[..]));
        assert!(e.take_referenced(), "a read marks the entry recently used");
    }

    #[test]
    fn clock_bit_is_taken_once() {
        let e = Entry::new(b"k", Val::resident(1, vec![]));
        assert!(e.take_referenced(), "starts set");
        assert!(!e.take_referenced(), "and taking it clears it");
        e.touch();
        assert!(e.take_referenced());
    }
}
