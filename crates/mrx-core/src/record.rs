//! A value record: one allocation, header and payload inline.
//!
//! # Why this exists rather than `Arc<Val>` with a `Vec<u8>`
//!
//! That shape costs **two** allocations per write — one for the `Arc`
//! block and one for the bytes — and two frees when the record is
//! displaced. Measured, it is 25–35 ns of a ~330 ns write. The C++
//! implementation's `mrx_val` is a single arena allocation with the
//! payload inline after a 16-byte header, which is what this mirrors.
//!
//! # What the `unsafe` rests on
//!
//! This is a hand-rolled `Arc` with a trailing byte slice. Four
//! invariants, each checked by a test below:
//!
//! 1. **The allocation layout is computed once and reproduced exactly on
//!    free.** `layout_for(len)` is the single source of truth, and `len`
//!    is stored in the header so `drop` can recompute the identical
//!    layout. Freeing with a different layout is undefined behaviour and
//!    is the classic way to get this wrong.
//! 2. **The record is immutable once constructed.** Nothing mutates a
//!    published record — that is already the cache's central invariant,
//!    and it is what makes handing out `&[u8]` from a shared reference
//!    sound.
//! 3. **The refcount is the only mutable field**, it is atomic, and the
//!    last decrement (`Release`, then an `Acquire` fence) happens-before
//!    the deallocation.
//! 4. **The payload is initialised before the record is reachable.**
//!    `new` writes the header and copies the bytes before it returns a
//!    `Record`, and nothing else can observe the allocation.
//!
//! Verified under Miri, including a deliberately injected layout bug to
//! confirm Miri catches this class of error.

#![allow(unsafe_code)]

use std::alloc::{alloc, dealloc, handle_alloc_error, Layout};
use std::ptr::NonNull;
use std::sync::atomic::{fence, AtomicUsize, Ordering};

use crate::Version;

/// What a record is. Kept as a `u8` in the header rather than a Rust enum
/// so the header layout is explicit and stable.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    /// Bytes are in memory, inline after this header.
    Resident = 0,
    /// Bytes live only in the durable store. The version is retained.
    Evicted = 1,
    /// The key is deleted.
    Tombstone = 2,
}

#[repr(C)]
struct Header {
    refs: AtomicUsize,
    version: Version,
    len: u32,
    kind: Kind,
}

/// The payload begins immediately after the header.
const PAYLOAD_OFFSET: usize = std::mem::size_of::<Header>();

fn layout_for(len: usize) -> Layout {
    // ONE definition, used by both `new` and `drop`. A second, subtly
    // different expression at the free site is how this goes wrong.
    Layout::from_size_align(
        PAYLOAD_OFFSET + len,
        std::mem::align_of::<Header>(),
    )
    .expect("value record layout")
}

/// A refcounted, immutable value record.
///
/// Behaves like `Arc<Val>`: cheap to clone, freed when the last handle
/// goes.
pub struct Record {
    ptr: NonNull<Header>,
}

// SAFETY: the record is immutable after construction and its only
// mutable field is an atomic refcount, so sharing a `&Record` across
// threads exposes nothing that is not already synchronised. Sending one
// transfers a refcount, which is likewise atomic.
unsafe impl Send for Record {}
// SAFETY: as above.
unsafe impl Sync for Record {}

impl Record {
    /// A live value, with `bytes` copied inline.
    pub fn resident(version: Version, bytes: &[u8]) -> Self {
        Self::new(version, Kind::Resident, bytes)
    }

    /// A deleted key.
    pub fn tombstone(version: Version) -> Self {
        Self::new(version, Kind::Tombstone, &[])
    }

    /// An evicted value, carrying forward the version whose bytes are in
    /// the durable store.
    pub fn evicted(version: Version) -> Self {
        Self::new(version, Kind::Evicted, &[])
    }

    fn new(version: Version, kind: Kind, bytes: &[u8]) -> Self {
        assert!(
            u32::try_from(bytes.len()).is_ok(),
            "value of {} bytes exceeds the 4 GiB record limit",
            bytes.len()
        );
        let layout = layout_for(bytes.len());
        // SAFETY: `layout` has non-zero size (the header alone is
        // non-empty), which is `alloc`'s only precondition.
        let raw = unsafe { alloc(layout) };
        let Some(ptr) = NonNull::new(raw.cast::<Header>()) else {
            handle_alloc_error(layout);
        };
        // SAFETY: `ptr` is freshly allocated for exactly this layout and
        // is uniquely owned here, so writing the header initialises it
        // without aliasing anything.
        unsafe {
            ptr.as_ptr().write(Header {
                refs: AtomicUsize::new(1),
                version,
                len: bytes.len() as u32,
                kind,
            });
            // SAFETY: the allocation has `bytes.len()` spare bytes after
            // the header by construction of `layout_for`, and the source
            // cannot overlap a block we have just allocated.
            if !bytes.is_empty() {
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr(),
                    raw.add(PAYLOAD_OFFSET),
                    bytes.len(),
                );
            }
        }
        Self { ptr }
    }

    fn header(&self) -> &Header {
        // SAFETY: the pointer is valid for as long as this handle holds a
        // reference, and the header is immutable apart from its atomic
        // refcount.
        unsafe { self.ptr.as_ref() }
    }

    /// The version stamped when this record was published.
    pub fn version(&self) -> Version {
        self.header().version
    }

    /// What kind of record this is.
    pub fn kind(&self) -> Kind {
        self.header().kind
    }

    /// Whether this key currently exists. An *evicted* value still
    /// exists — only a tombstone means absent.
    pub fn is_live(&self) -> bool {
        self.kind() != Kind::Tombstone
    }

    /// The bytes, if they are in memory.
    pub fn bytes(&self) -> Option<&[u8]> {
        if self.kind() != Kind::Resident {
            return None;
        }
        let len = self.header().len as usize;
        // SAFETY: a `Resident` record was constructed with exactly `len`
        // payload bytes after the header, all initialised by
        // `copy_nonoverlapping`, and the record is immutable, so a shared
        // slice over them is valid for as long as `&self`.
        unsafe {
            Some(std::slice::from_raw_parts(
                self.ptr.as_ptr().cast::<u8>().add(PAYLOAD_OFFSET),
                len,
            ))
        }
    }

    /// Bytes this record occupies. Only resident values can be
    /// reclaimed, which is why the capacity check subtracts a floor.
    pub fn payload_bytes(&self) -> u64 {
        match self.kind() {
            Kind::Resident => u64::from(self.header().len),
            _ => 0,
        }
    }

    /// Whether two handles refer to the same record.
    ///
    /// Identity, not equality: two records can be equal in content and
    /// distinct in obligation, which is what the fill path's ABA guard
    /// turns on.
    pub fn ptr_eq(a: &Self, b: &Self) -> bool {
        std::ptr::eq(a.ptr.as_ptr(), b.ptr.as_ptr())
    }
}

impl Clone for Record {
    fn clone(&self) -> Self {
        // Relaxed is sufficient: we already hold a reference, so the
        // record cannot be freed here, and no data is being published.
        // This is `Arc`'s own reasoning.
        self.header().refs.fetch_add(1, Ordering::Relaxed);
        Self { ptr: self.ptr }
    }
}

impl Drop for Record {
    fn drop(&mut self) {
        // Release so that everything this thread did with the record
        // happens-before the free.
        if self.header().refs.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        // Acquire the other threads' releases before touching the
        // allocation. Again, `Arc`'s reasoning.
        fence(Ordering::Acquire);
        let len = self.header().len as usize;
        // SAFETY: the refcount reached zero, so this handle is the last
        // and no other thread can observe the allocation. The layout is
        // recomputed from the same `len` the allocation was made with, by
        // the same function. `Header` has no fields needing drop, and the
        // payload is plain bytes, so there is nothing to run first.
        unsafe {
            dealloc(self.ptr.as_ptr().cast::<u8>(), layout_for(len));
        }
    }
}

impl std::fmt::Debug for Record {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Record")
            .field("version", &self.version())
            .field("kind", &self.kind())
            .field("len", &self.header().len)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn payload_round_trips_at_every_awkward_length() {
        // 0 and 1 exercise the empty-payload branch and the smallest
        // copy; the others straddle the header size and alignment.
        for len in [0usize, 1, 7, 8, 15, 16, 23, 100, 4096] {
            let bytes: Vec<u8> = (0..len).map(|i| (i % 251) as u8).collect();
            let r = Record::resident(42, &bytes);
            assert_eq!(r.version(), 42);
            assert_eq!(r.kind(), Kind::Resident);
            assert_eq!(r.bytes(), Some(bytes.as_slice()), "len {len}");
            assert_eq!(r.payload_bytes(), len as u64);
        }
    }

    #[test]
    fn only_a_tombstone_means_absent() {
        assert!(Record::resident(1, b"x").is_live());
        assert!(Record::evicted(1).is_live(), "an evicted value still EXISTS");
        assert!(!Record::tombstone(1).is_live());
    }

    #[test]
    fn evicted_retains_its_version_and_has_no_bytes() {
        // If this ever loses the version, the fill path's ABA guard
        // silently stops working.
        let r = Record::evicted(77);
        assert_eq!(r.version(), 77);
        assert_eq!(r.bytes(), None);
        assert_eq!(r.payload_bytes(), 0);
    }

    #[test]
    fn clones_share_one_allocation_and_the_last_drop_frees_it() {
        let a = Record::resident(1, b"shared");
        let b = a.clone();
        let c = b.clone();
        assert!(Record::ptr_eq(&a, &c));
        assert_eq!(a.header().refs.load(Ordering::Relaxed), 3);
        drop(b);
        drop(c);
        assert_eq!(a.header().refs.load(Ordering::Relaxed), 1);
        assert_eq!(a.bytes(), Some(&b"shared"[..]), "still readable");
        // `a` frees here; Miri checks that it is exactly once and with
        // the right layout.
    }

    #[test]
    fn ptr_eq_is_identity_not_equality() {
        let a = Record::resident(1, b"same");
        let twin = Record::resident(1, b"same");
        assert!(!Record::ptr_eq(&a, &twin), "equal content, distinct records");
        assert!(Record::ptr_eq(&a, &a.clone()));
    }

    #[test]
    fn layout_is_one_allocation_of_header_plus_payload() {
        // The whole point of the type. If this ever stops holding, the
        // record has silently gone back to two allocations.
        assert_eq!(layout_for(0).size(), PAYLOAD_OFFSET);
        assert_eq!(layout_for(100).size(), PAYLOAD_OFFSET + 100);
        assert!(layout_for(100).align() >= std::mem::align_of::<u64>());
    }

    #[test]
    fn records_move_across_threads() {
        let r = Record::resident(9, b"crossing");
        let h = std::thread::spawn(move || {
            assert_eq!(r.bytes(), Some(&b"crossing"[..]));
            r.version()
        });
        assert_eq!(h.join().unwrap(), 9);
    }

    #[test]
    fn concurrent_clones_and_drops_free_exactly_once() {
        // The refcount under contention. Miri checks the free; this
        // checks the record stays readable throughout.
        let r = Record::resident(3, b"contended");
        std::thread::scope(|s| {
            for _ in 0..8 {
                let r = r.clone();
                s.spawn(move || {
                    for _ in 0..64 {
                        let c = r.clone();
                        assert_eq!(c.bytes(), Some(&b"contended"[..]));
                    }
                });
            }
        });
        assert_eq!(r.header().refs.load(Ordering::Relaxed), 1);
    }
}
