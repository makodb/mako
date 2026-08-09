#![allow(unsafe_code)]

// Test-local stand-in for the concurrently prepared owner.  The production
// module uses this exact crate path and will resolve it through the normal
// manifest dependency once `legacy_basetypes` is wired into `base::mod`.
mod base {
    pub mod legacy_basetypes {
        use std::sync::OnceLock;
        use std::time::Instant;

        pub const RRR_USEC_PER_SEC: u64 = 1_000_000_u64;

        pub struct Time;

        impl Time {
            pub fn now(_accurate: bool) -> u64 {
                static EPOCH: OnceLock<Instant> = OnceLock::new();
                EPOCH.get_or_init(Instant::now).elapsed().as_micros() as u64
            }
        }
    }
}

#[path = "../src/runtime/legacy_fiber.rs"]
mod legacy_fiber;

use base::legacy_basetypes::Time;
use legacy_fiber::cpp::rrr::reactor::{take_test_sleep_calls, with_test_fiber};
use legacy_fiber::this_fiber;

#[test]
fn outside_context_surface_is_empty_and_yield_is_a_noop() {
    assert_eq!(this_fiber::get_id(), 0_u64);
    assert!(this_fiber::current().is_none());
    assert!(!this_fiber::in_fiber_context());
    this_fiber::r#yield();
}

#[test]
fn installed_context_exposes_identity_current_handle_and_yield() {
    let ((id, present, inside), yields) = with_test_fiber(73_u64, || {
        (
            this_fiber::get_id(),
            this_fiber::current().is_some(),
            this_fiber::in_fiber_context(),
        )
    });
    assert_eq!(id, 73_u64);
    assert!(present);
    assert!(inside);
    assert_eq!(yields, 0_u64);

    let (_, yields) = with_test_fiber(91_u64, || {
        this_fiber::r#yield();
        this_fiber::r#yield();
    });
    assert_eq!(yields, 2_u64);
    assert!(!this_fiber::in_fiber_context());
}

#[test]
fn sleep_unit_conversions_delegate_exact_microseconds() {
    let _ = take_test_sleep_calls();
    this_fiber::sleep_us(17_u64);
    this_fiber::sleep_ms(23_u64);
    this_fiber::sleep_s(2_u64);
    assert_eq!(take_test_sleep_calls(), [17_u64, 23_000_u64, 2_000_000_u64]);
}

#[test]
fn sleep_until_skips_past_deadline_and_delegates_future_delta() {
    let _ = take_test_sleep_calls();
    let now = Time::now(true);
    this_fiber::sleep_until_us(now.saturating_sub(1_u64));
    assert!(take_test_sleep_calls().is_empty());

    let deadline = Time::now(true) + 50_000_u64;
    this_fiber::sleep_until_us(deadline);
    let calls = take_test_sleep_calls();
    assert_eq!(calls.len(), 1_usize);
    assert!(calls[0] > 0_u64);
    assert!(calls[0] <= 50_000_u64);
}

#[test]
fn exact_namespace_surface_and_dependency_paths_are_pinned() {
    let owner = include_str!("../src/runtime/legacy_fiber.rs");
    assert!(owner.contains("pub mod this_fiber"));
    assert!(owner.contains("crate::base::legacy_basetypes::{Time, RRR_USEC_PER_SEC}"));
    assert!(owner.contains("use cpp::rrr::reactor as cpp_reactor"));

    for function in [
        "pub fn get_id()",
        "pub fn current()",
        "pub fn in_fiber_context()",
        "pub fn r#yield()",
        "pub fn sleep_us(",
        "pub fn sleep_ms(",
        "pub fn sleep_s(",
        "pub fn sleep_until_us(",
    ] {
        assert_eq!(
            owner.matches(function).count(),
            1_usize,
            "unexpected surface for {function}"
        );
    }

    assert!(owner.contains("cpp_reactor::Fiber::current_fiber()"));
    assert!(owner.contains("cpp_reactor::Fiber::yield_(fiber)"));
    assert!(owner.contains("cpp_reactor::fiber_sleep(microseconds)"));
}
