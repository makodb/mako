//! Optional HLC write-prefetch hint for restricted commit ordering.

use mako_clock::Clock;

/// Resolve the optional CPU capability on the cold worker-attachment path.
pub(crate) fn initialize() {
    #[cfg(all(any(target_arch = "x86", target_arch = "x86_64"), not(miri)))]
    let _ = supported();
}

/// Hint only. No timestamp, gate, or application value is read or modified.
#[inline]
pub(crate) fn restricted_clock(clock: &Clock) {
    #[cfg(all(any(target_arch = "x86", target_arch = "x86_64"), not(miri)))]
    if supported() {
        let address = clock.prefetch_address();
        // SAFETY: `clock` remains borrowed, alive and unmoved through this
        // instruction. PRFCHW was checked below. PREFETCHW is a non-faulting
        // cache hint, not an access to or mutation of the packed clock word.
        unsafe {
            core::arch::asm!(
                "prefetchw [{address}]",
                address = in(reg) address,
                options(nostack, preserves_flags, readonly)
            );
        }
    }
    #[cfg(any(not(any(target_arch = "x86", target_arch = "x86_64")), miri))]
    let _ = clock;
}

#[cfg(all(any(target_arch = "x86", target_arch = "x86_64"), not(miri)))]
#[allow(unused_unsafe)]
#[inline]
fn supported() -> bool {
    use std::sync::atomic::{AtomicU8, Ordering};

    // Do not introduce a hot capability read on the packed HLC's cache line.
    #[repr(align(128))]
    struct Support(AtomicU8);
    static SUPPORT: Support = Support(AtomicU8::new(0));

    match SUPPORT.0.load(Ordering::Relaxed) {
        1 => false,
        2 => true,
        _ => {
            #[cfg(target_arch = "x86")]
            use core::arch::x86::__cpuid;
            #[cfg(target_arch = "x86_64")]
            use core::arch::x86_64::__cpuid;

            // SAFETY: supported Rust x86 targets have CPUID. Query the
            // extended feature leaf only when its maximum admits it.
            let maximum = unsafe { __cpuid(0x8000_0000) }.eax;
            let has_hint =
                maximum >= 0x8000_0001 && unsafe { __cpuid(0x8000_0001) }.ecx & (1 << 8) != 0;
            SUPPORT
                .0
                .store(if has_hint { 2 } else { 1 }, Ordering::Relaxed);
            has_hint
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn hint_does_not_allocate_a_stamp_or_change_the_gate() {
        let clock = mako_clock::Clock::new();
        super::initialize();
        super::restricted_clock(&clock);
        assert_eq!(clock.current_stamp(), 0);
        assert!(!clock.general_gate_held());
        clock.observe_stamp(42);
        let stamp = 42;
        assert_eq!(clock.current_stamp(), stamp);
        assert_eq!(clock.try_allocate_sequence(), Some(1));
        super::restricted_clock(&clock);
        assert_eq!(clock.current_stamp(), stamp);
        assert_eq!(clock.try_allocate_sequence(), Some(2));
        clock.enter_general();
        super::restricted_clock(&clock);
        assert_eq!(clock.current_stamp(), stamp);
        assert!(clock.general_gate_held());
        assert_eq!(clock.try_allocate_sequence(), Some(3));
        clock.leave_general();
    }
}
