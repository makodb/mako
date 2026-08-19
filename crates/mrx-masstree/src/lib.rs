//! [`KeyIndex`] over masstree, through the `mtx_*` C ABI.
//!
//! This crate is the *only* place in the Rust cache that contains
//! `unsafe`, and it is deliberately thin: it converts slices to pointers,
//! checks status codes, and copies results out. Every decision that could
//! be made one level up was.
//!
//! # Threads are a capped, permanent resource
//!
//! [`mtx_thread_attach`](mtree_sys::mtx_thread_attach) allocates a core
//! ID from a 512-entry per-runtime space, and IDs are **never recycled**:
//! a dead thread's slot and its deferred-free queue stay allocated
//! forever. So this crate attaches lazily and caches the fact in a
//! `thread_local` with **no `Drop`** — there is nothing to release, and a
//! destructor here would only create a window where a thread believes it
//! is detached while masstree still holds its ID.
//!
//! The practical consequence for callers: use a fixed pool of long-lived
//! threads. Spawning a thread per request will exhaust the space and
//! every call from thread 513 onward fails with
//! [`IndexError::NoCoreId`].
//!
//! # RCU is not visible here
//!
//! The C ABI opens and closes an epoch inside each call, and nothing that
//! escapes a call depends on an epoch still being held — keys are copied
//! into a caller arena rather than borrowed from tree memory. That is why
//! this crate can hand masstree results to arbitrary Rust code, including
//! code that does IO, without freezing reclamation process-wide.

use std::cell::Cell;
use std::fmt;

use mrx_core::{EntryWord, KeyIndex};
use mtree_sys as sys;

/// A failure from the C ABI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexError {
    /// The 512-per-runtime core-ID space is exhausted.
    NoCoreId,
    /// The calling thread is bound to a non-global `SiloRuntime`.
    WrongRuntime,
    /// A null or otherwise invalid argument reached the ABI.
    Invalid,
    /// A C++ exception was contained at the boundary.
    Internal,
    /// Allocation failed while creating the tree.
    OutOfMemory,
    /// The linked C++ does not match what this crate was compiled for.
    AbiMismatch(&'static str),
    /// An unrecognised status code.
    Unknown(i32),
}

impl fmt::Display for IndexError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoCoreId => write!(
                f,
                "masstree core-ID space exhausted (512 per runtime, never \
                 recycled); use a fixed pool of long-lived threads"
            ),
            Self::WrongRuntime => {
                write!(f, "calling thread is bound to a non-global SiloRuntime")
            }
            Self::Invalid => write!(f, "invalid argument passed to the mtx ABI"),
            Self::Internal => write!(f, "a C++ exception was contained at the boundary"),
            Self::OutOfMemory => write!(f, "masstree allocation failed"),
            Self::AbiMismatch(m) => write!(f, "{m}"),
            Self::Unknown(c) => write!(f, "unrecognised mtx status {c}"),
        }
    }
}

impl std::error::Error for IndexError {}

fn check(status: i32) -> Result<(), IndexError> {
    match status {
        sys::MTX_OK => Ok(()),
        sys::MTX_ERR_NO_CORE_ID => Err(IndexError::NoCoreId),
        sys::MTX_ERR_WRONG_RUNTIME => Err(IndexError::WrongRuntime),
        sys::MTX_ERR_INVALID => Err(IndexError::Invalid),
        sys::MTX_ERR_INTERNAL => Err(IndexError::Internal),
        // NOT_ATTACHED and NO_SPACE are handled at their call sites,
        // where they are recoverable rather than errors.
        other => Err(IndexError::Unknown(other)),
    }
}

thread_local! {
    /// Whether this thread has been through `mtx_thread_attach`.
    ///
    /// A plain `Cell<bool>` with no destructor. See the module note.
    static ATTACHED: Cell<bool> = const { Cell::new(false) };
}

/// Register the calling thread if it is not already registered.
///
/// Idempotent and cheap after the first call.
pub fn attach_thread() -> Result<(), IndexError> {
    ATTACHED.with(|a| {
        if a.get() {
            return Ok(());
        }
        // SAFETY: no preconditions; idempotent on the C++ side too.
        check(unsafe { sys::mtx_thread_attach() })?;
        a.set(true);
        Ok(())
    })
}

/// A masstree, usable as the cache's key directory.
pub struct MasstreeIndex {
    tree: *mut sys::MtxTree,
}

// SAFETY: masstree is a concurrent structure designed for exactly this —
// many threads reading and writing the same tree, each pinned in its own
// RCU epoch. The C ABI opens and closes those epochs per call, and every
// call takes the tree by raw pointer rather than through any per-thread
// state attached to it. The one per-thread requirement is registration,
// which `attach_thread` enforces on whatever thread makes the call.
unsafe impl Send for MasstreeIndex {}
// SAFETY: as above.
unsafe impl Sync for MasstreeIndex {}

impl MasstreeIndex {
    /// Create a tree, checking the ABI first.
    ///
    /// The version check alone would not be enough: struct layout drift
    /// is what a version bump misses, and it corrupts silently, so
    /// [`mtree_sys::assert_abi_matches`] checks the size of `mtx_kv` too.
    pub fn new() -> Result<Self, IndexError> {
        sys::assert_abi_matches().map_err(IndexError::AbiMismatch)?;
        attach_thread()?;
        // SAFETY: no preconditions; returns null on allocation failure.
        let tree = unsafe { sys::mtx_create() };
        if tree.is_null() {
            return Err(IndexError::OutOfMemory);
        }
        Ok(Self { tree })
    }

    /// Look up a key, reporting failures instead of swallowing them.
    pub fn try_get(&self, key: &[u8]) -> Result<Option<EntryWord>, IndexError> {
        attach_thread()?;
        let mut out: u64 = sys::MTX_WORD_NULL;
        // SAFETY: `tree` is non-null for the life of `self`; `key` is a
        // valid slice, and the ABI copies what it needs before returning.
        check(unsafe {
            sys::mtx_get(
                self.tree,
                key.as_ptr() as *const core::ffi::c_char,
                key.len(),
                &mut out,
            )
        })?;
        Ok(if out == sys::MTX_WORD_NULL { None } else { Some(out) })
    }

    /// Install `word` if absent, reporting failures.
    pub fn try_get_or_insert(
        &self,
        key: &[u8],
        word: EntryWord,
    ) -> Result<EntryWord, IndexError> {
        debug_assert_ne!(
            word,
            sys::MTX_WORD_NULL,
            "word 0 is reserved for absence and would read back as a miss"
        );
        attach_thread()?;
        let mut out: u64 = sys::MTX_WORD_NULL;
        // `insert_if_absent`, NOT `get_or_insert`.
        //
        // The cache calls this only after its own lookup has missed, so
        // `get_or_insert`'s leading probe would re-walk the tree to
        // re-discover that miss — three traversals per insert against the
        // C++ implementation's two, which callgrind attributed as the
        // whole of the Rust insert path's deficit.
        //
        // SAFETY: as `try_get`.
        check(unsafe {
            sys::mtx_insert_if_absent(
                self.tree,
                key.as_ptr() as *const core::ffi::c_char,
                key.len(),
                word,
                &mut out,
            )
        })?;
        Ok(out)
    }

    /// Approximate key count.
    pub fn try_len(&self) -> Result<usize, IndexError> {
        attach_thread()?;
        let mut out: usize = 0;
        // SAFETY: as `try_get`.
        check(unsafe { sys::mtx_size(self.tree, &mut out) })?;
        Ok(out)
    }

    /// One range walk, ascending or descending.
    ///
    /// A short result is a **chunk boundary, not necessarily
    /// end-of-range**: the walk stops as soon as either the entry buffer
    /// or the key arena fills. Treating short-means-done drops the tail
    /// of every scan whose keys happen to be long, which is why the arena
    /// is grown and retried here rather than left to the caller.
    fn walk(
        &self,
        from: &[u8],
        budget: usize,
        ascending: bool,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> Result<usize, IndexError> {
        if budget == 0 {
            return Ok(0);
        }
        attach_thread()?;
        let mut kvs: Vec<sys::MtxKv> = vec![
            sys::MtxKv { key_off: 0, key_len: 0, word: 0 };
            budget
        ];
        // 64 bytes a key to start. Grown below rather than guessed at,
        // because a wrong guess here is silent truncation.
        let mut arena: Vec<u8> = vec![0u8; budget.saturating_mul(64).max(4096)];

        loop {
            let mut n_out: usize = 0;
            let mut used: usize = 0;
            let f = if ascending {
                sys::mtx_scan_chunk
            } else {
                sys::mtx_rscan_chunk
            };
            // SAFETY: `kvs` and `arena` are live, correctly sized, and
            // exclusively borrowed for the duration of the call; the ABI
            // writes at most `cap` entries and `arena_cap` bytes and
            // reports how many of each it used.
            let st = unsafe {
                f(
                    self.tree,
                    from.as_ptr() as *const core::ffi::c_char,
                    from.len(),
                    kvs.as_mut_ptr(),
                    kvs.len(),
                    arena.as_mut_ptr() as *mut core::ffi::c_char,
                    arena.len(),
                    &mut n_out,
                    &mut used,
                )
            };
            if st == sys::MTX_ERR_NO_SPACE {
                // Nothing fit at all. `used` is what one key needs.
                arena.resize(used.max(arena.len().saturating_mul(2)), 0);
                continue;
            }
            check(st)?;

            // `used > arena.len()` is the ABI's arena-full signal, and
            // the value is what one more key would have needed. Grow to
            // exactly that and re-walk from the same `from`.
            //
            // Guessing a margin here instead (`used + 64 > arena.len()`)
            // is what the first version did, and it silently dropped 980
            // of 1000 keys the moment keys were longer than the guess:
            // the walk stopped 112 bytes short of full, which the margin
            // read as end-of-range.
            if used > arena.len() {
                arena.resize(used.max(arena.len().saturating_mul(2)), 0);
                continue;
            }

            for kv in &kvs[..n_out] {
                let s = kv.key_off as usize;
                let e = s + kv.key_len as usize;
                out.push((arena[s..e].to_vec(), kv.word));
            }
            return Ok(n_out);
        }
    }
}

impl Drop for MasstreeIndex {
    fn drop(&mut self) {
        // SAFETY: `tree` came from `mtx_create` and is dropped once.
        unsafe { sys::mtx_destroy(self.tree) };
    }
}

/// The [`KeyIndex`] surface.
///
/// The trait is infallible by design — the cache above it treats an index
/// miss as authoritative absence and has nowhere to put an error — so
/// these panic rather than paper over a failure. Every one of them means
/// the process is already in an unrecoverable state: an exhausted core-ID
/// space, an ABI mismatch, or a contained C++ exception. Use the `try_*`
/// methods where a caller can actually respond.
impl KeyIndex for MasstreeIndex {
    fn get(&self, key: &[u8]) -> Option<EntryWord> {
        self.try_get(key).expect("masstree lookup failed")
    }

    fn get_or_insert(&self, key: &[u8], word: EntryWord) -> EntryWord {
        self.try_get_or_insert(key, word)
            .expect("masstree insert failed")
    }

    fn scan_chunk(
        &self,
        from: &[u8],
        budget: usize,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> usize {
        self.walk(from, budget, true, out).expect("masstree scan failed")
    }

    fn rscan_chunk(
        &self,
        from: &[u8],
        budget: usize,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> usize {
        self.walk(from, budget, false, out)
            .expect("masstree reverse scan failed")
    }

    fn len(&self) -> usize {
        self.try_len().expect("masstree size failed")
    }
}
