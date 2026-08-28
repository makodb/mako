//! One audited architecture hint for the stable Rust record arena.

// The public adapter remains entirely safe. This private module contains the
// one target-specific intrinsic needed to recover Masstree's original
// early-value-prefetch behavior for integral RecordIds.
#![allow(unsafe_code)]

/// Hints that the cache line containing `target` will be read shortly.
///
/// The reference proves that the hinted address is valid for this call. The
/// intrinsic neither creates a Rust reference nor makes correctness depend on
/// the hint; unsupported architectures deliberately use a no-op fallback.
#[inline(always)]
pub(super) fn read<T>(target: &T) {
    #[cfg(target_arch = "x86")]
    // SAFETY: `target` is a live Rust reference and therefore supplies a valid
    // readable address for the duration of this synchronous prefetch hint.
    unsafe {
        std::arch::x86::_mm_prefetch(
            std::ptr::from_ref(target).cast::<i8>(),
            std::arch::x86::_MM_HINT_T0,
        );
    }

    #[cfg(target_arch = "x86_64")]
    // SAFETY: `target` is a live Rust reference and therefore supplies a valid
    // readable address for the duration of this synchronous prefetch hint.
    unsafe {
        std::arch::x86_64::_mm_prefetch(
            std::ptr::from_ref(target).cast::<i8>(),
            std::arch::x86_64::_MM_HINT_T0,
        );
    }

    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    let _ = target;
}
