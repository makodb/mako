//! Raw FFI declarations for the `mtx_*` C ABI over masstree
//! (`src/mako/storage/mtree_abi.h`).
//!
//! This crate is declarations and nothing else. It deliberately does not
//! wrap, allocate, or interpret — see [`crate`-level notes in
//! `README.md`](../README.md) for why the boundary is shaped this way,
//! particularly the RCU epoch rule and why the stored value is an opaque
//! word.
//!
//! # Hand-written, not bindgen'd
//!
//! The header is a dozen declarations and changes rarely, whereas
//! bindgen would add a libclang dependency whose version must agree with
//! a build that hard-requires a specific Clang. Trading a stable
//! hand-written file for a moving build-time dependency is a bad deal at
//! this size. [`assert_abi_matches`] is what keeps the two in step.
//!
//! # `extern "C"`, never `extern "C-unwind"`
//!
//! Every one of these functions is `noexcept` on the C++ side with a
//! `catch (...)` at its boundary, so no exception can reach Rust. That
//! guard has to live in C++: a foreign C++ exception unwinding a Rust
//! frame compiled with `panic = "abort"` is undefined behaviour, and
//! `std::panic::catch_unwind` does not catch C++ exceptions.

#![no_std]

use core::ffi::c_char;

/// Bumped on any incompatible change to the C ABI.
pub const MTX_ABI_VERSION: u32 = 4;

/// Success.
pub const MTX_OK: i32 = 0;
/// The per-runtime core-ID space (512) is exhausted.
pub const MTX_ERR_NO_CORE_ID: i32 = 1;
/// [`mtx_thread_attach`] was not called on this thread.
pub const MTX_ERR_NOT_ATTACHED: i32 = 2;
/// The calling thread is bound to a non-global `SiloRuntime`.
pub const MTX_ERR_WRONG_RUNTIME: i32 = 3;
/// A null or otherwise invalid argument.
pub const MTX_ERR_INVALID: i32 = 4;
/// The caller's buffer or arena was too small to make any progress.
pub const MTX_ERR_NO_SPACE: i32 = 5;
/// A C++ exception was contained at the boundary.
pub const MTX_ERR_INTERNAL: i32 = 6;

/// Reserved word meaning "absent". Never a valid stored value.
pub const MTX_WORD_NULL: u64 = 0;

/// Opaque tree handle.
///
/// Deliberately an opaque type rather than a type alias for a C++
/// struct: nothing above this crate should be able to name, construct,
/// or dereference it.
#[repr(C)]
pub struct MtxTree {
    _private: [u8; 0],
}

/// One key/word pair from a range walk.
///
/// `key_off` is a byte offset into the caller's arena, not a pointer —
/// keys are copied out precisely so that nothing borrowed from tree
/// memory survives the call.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MtxKv {
    /// Offset of this key's bytes within the caller-supplied arena.
    pub key_off: u32,
    /// Length of this key in bytes.
    pub key_len: u32,
    /// The opaque stored word.
    pub word: u64,
}

extern "C" {
    /// Runtime ABI version, to be compared against [`MTX_ABI_VERSION`].
    pub fn mtx_abi_version() -> u32;
    /// Runtime `sizeof(mtx_kv)`, to be compared against
    /// `size_of::<MtxKv>()`.
    pub fn mtx_kv_size() -> usize;

    /// Register the calling thread. Idempotent; required once per
    /// thread before any other call from that thread.
    ///
    /// Threads are a permanent, capped resource here: core IDs are never
    /// recycled and a dead thread's deferred-free queue is never
    /// drained. Use a fixed set of long-lived threads.
    pub fn mtx_thread_attach() -> i32;

    /// Create a tree. Returns null on allocation failure.
    pub fn mtx_create() -> *mut MtxTree;
    /// Destroy a tree. Null is a no-op.
    pub fn mtx_destroy(t: *mut MtxTree);

    /// Look up a key; `*out` receives the word or [`MTX_WORD_NULL`].
    pub fn mtx_get(t: *mut MtxTree, key: *const c_char, klen: usize, out: *mut u64) -> i32;

    /// Install `word` if absent; `*out` receives whichever word is now
    /// associated with the key.
    pub fn mtx_get_or_insert(
        t: *mut MtxTree,
        key: *const c_char,
        klen: usize,
        word: u64,
        out: *mut u64,
    ) -> i32;

    /// Hold ONE RCU region across several calls on this thread.
    ///
    /// The first region on a thread takes the core's ticker spinlock and
    /// reads the clock; nested ones skip both. Pinning once around a run
    /// of tree calls pays that setup once instead of per call.
    ///
    /// **Never hold a pin across IO or a blocking call.** An open region
    /// pins this core's ticker slot, which the ticker daemon needs to
    /// advance every lagging core, so holding one across a disk read
    /// stalls reclamation process-wide. Reentrant.
    pub fn mtx_region_pin() -> i32;

    /// Release a pin taken by [`mtx_region_pin`].
    pub fn mtx_region_unpin();

    /// As [`mtx_get_or_insert`], but without the leading probe.
    ///
    /// For a caller that has ALREADY established the key is absent. The
    /// probe would re-walk the tree to re-discover a miss the caller just
    /// found, which is what made this crate's insert path cost three
    /// masstree traversals against the C++ implementation's two.
    pub fn mtx_insert_if_absent(
        t: *mut MtxTree,
        key: *const c_char,
        klen: usize,
        word: u64,
        out: *mut u64,
    ) -> i32;

    /// Ascending range walk into caller storage.
    ///
    /// A short result means a chunk boundary, not necessarily
    /// end-of-range, and `arena_used` says which:
    ///
    /// * `<= arena_cap` — bytes consumed; the walk ended because the
    ///   range ended or `cap` was reached.
    /// * `>  arena_cap` — the walk stopped for want of arena space, and
    ///   this is what one more key would have needed. Grow and re-call
    ///   with the same `from`.
    ///
    /// Ignoring that distinction truncates every scan whose keys happen
    /// to be long — 980 of 1000 in the test that found it.
    pub fn mtx_scan_chunk(
        t: *mut MtxTree,
        from: *const c_char,
        fromlen: usize,
        out: *mut MtxKv,
        cap: usize,
        arena: *mut c_char,
        arena_cap: usize,
        n_out: *mut usize,
        arena_used: *mut usize,
    ) -> i32;

    /// Descending mirror of [`mtx_scan_chunk`].
    pub fn mtx_rscan_chunk(
        t: *mut MtxTree,
        from: *const c_char,
        fromlen: usize,
        out: *mut MtxKv,
        cap: usize,
        arena: *mut c_char,
        arena_cap: usize,
        n_out: *mut usize,
        arena_used: *mut usize,
    ) -> i32;

    /// Approximate key count.
    pub fn mtx_size(t: *mut MtxTree, out: *mut usize) -> i32;
}

/// Check the linked C ABI against what this crate was compiled for.
///
/// Call once at store open. The version number alone is not enough: a
/// struct layout drift is the failure mode a version bump would miss,
/// and it corrupts silently, so the size is checked too.
///
/// # Safety
///
/// Calls into the C ABI, which is safe to call at any time.
pub fn assert_abi_matches() -> Result<(), &'static str> {
    // SAFETY: both are pure accessors with no preconditions.
    let (ver, kv) = unsafe { (mtx_abi_version(), mtx_kv_size()) };
    if ver != MTX_ABI_VERSION {
        return Err("mtx ABI version mismatch between Rust and the linked C++");
    }
    if kv != core::mem::size_of::<MtxKv>() {
        return Err("mtx_kv layout mismatch between Rust and the linked C++");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // The C++ side is linked by CMake, not by cargo, so a plain `cargo
    // test` here has no symbols to call. These checks are therefore
    // pure-Rust layout assertions; the real round-trip lives in the
    // gtest suite (tests/test_mtree_abi.cc) and later in the
    // differential harness.

    #[test]
    fn abi_version_is_four() {
        // 1 -> 2 when `arena_used` gained its overflow meaning; 2 -> 3
        // when mtx_insert_if_absent was added; 3 -> 4 for region pinning.
        // The runtime check in assert_abi_matches is what actually
        // enforces agreement; this pins the constant against an
        // accidental edit.
        assert_eq!(MTX_ABI_VERSION, 4);
    }

    #[test]
    fn kv_layout_is_what_the_c_header_declares() {
        assert_eq!(core::mem::size_of::<MtxKv>(), 16);
        assert_eq!(core::mem::align_of::<MtxKv>(), 8);
    }

    #[test]
    fn word_null_is_reserved() {
        assert_eq!(MTX_WORD_NULL, 0);
    }

    #[test]
    fn status_codes_are_distinct() {
        let codes = [
            MTX_OK,
            MTX_ERR_NO_CORE_ID,
            MTX_ERR_NOT_ATTACHED,
            MTX_ERR_WRONG_RUNTIME,
            MTX_ERR_INVALID,
            MTX_ERR_NO_SPACE,
            MTX_ERR_INTERNAL,
        ];
        for (i, a) in codes.iter().enumerate() {
            for b in &codes[i + 1..] {
                assert_ne!(a, b, "status codes must be distinct");
            }
        }
    }
}
