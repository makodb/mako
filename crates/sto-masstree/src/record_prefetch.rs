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
#[cfg(feature = "fixed-u64")]
pub(super) fn read<T>(target: &T) {
    read_address(std::ptr::from_ref(target));
}

/// Hints that the cache line at `target` will be read shortly.
///
/// Unlike an ordinary raw-pointer load, the architecture intrinsic is defined
/// as a non-trapping hint even when the address is invalid. This lets the
/// private direct-token lane overlap cache fill with its existing, later slot
/// validation without constructing a reference or dereferencing the token.
#[inline(always)]
pub(super) fn read_address<T>(target: *const T) {
    #[cfg(target_arch = "x86")]
    if std::arch::is_x86_feature_detected!("sse") {
        // SAFETY: the runtime check establishes the intrinsic's SSE target
        // feature. Invalid raw pointers are explicitly permitted here because
        // `_mm_prefetch` is a non-trapping hint and performs no dereference.
        unsafe {
            std::arch::x86::_mm_prefetch(target.cast::<i8>(), std::arch::x86::_MM_HINT_T0);
        }
    }

    #[cfg(target_arch = "x86_64")]
    // SAFETY: SSE is part of the x86-64 baseline. Invalid raw pointers are
    // explicitly permitted because `_mm_prefetch` is a non-trapping hint and
    // performs no dereference.
    unsafe {
        std::arch::x86_64::_mm_prefetch(target.cast::<i8>(), std::arch::x86_64::_MM_HINT_T0);
    }

    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
    let _ = target;
}
