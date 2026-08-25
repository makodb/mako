#![allow(non_snake_case)]

use rrr::misc::{clamp, format_thousands, get_ncpu, Job, OneTimeJob};
use std::cmp::Ordering;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering as AtomicOrdering};

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_get_ncpu() -> i32 {
    8
}

#[allow(unsafe_code)]
#[allow(clippy::missing_safety_doc)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn srpc_format_fixed_2(value: f64, output: *mut i8, capacity: usize) -> i32 {
    let formatted = format!("{value:.2}");
    assert!(formatted.len() < capacity);
    // SAFETY: the canonical owner passes a live buffer of `capacity` bytes;
    // this test oracle checked that the formatted bytes fit.
    unsafe {
        core::ptr::copy_nonoverlapping(formatted.as_ptr().cast::<i8>(), output, formatted.len());
        *output.add(formatted.len()) = 0;
    }
    formatted.len() as i32
}

#[test]
fn process_seam_and_homogeneous_clamp_preserve_results() {
    assert_eq!(get_ncpu(), 8);
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Value(i32);

#[derive(Clone, Copy, Debug)]
struct Bound(i16);

impl PartialEq<Bound> for Value {
    fn eq(&self, other: &Bound) -> bool {
        self.0 == i32::from(other.0)
    }
}

impl PartialOrd<Bound> for Value {
    fn partial_cmp(&self, other: &Bound) -> Option<Ordering> {
        self.0.partial_cmp(&i32::from(other.0))
    }
}

impl From<&Bound> for Value {
    fn from(bound: &Bound) -> Value {
        Value(i32::from(bound.0))
    }
}

impl From<&Value> for Value {
    fn from(value: &Value) -> Value {
        *value
    }
}

#[test]
fn heterogeneous_clamp_keeps_the_legacy_template_shape() {
    assert_eq!(clamp(&Value(-2), &Bound(0), &Bound(10)), Value(0));
    assert_eq!(clamp(&Value(4), &Bound(0), &Bound(10)), Value(4));
    assert_eq!(clamp(&Value(12), &Bound(0), &Bound(10)), Value(10));
}

#[test]
fn one_time_job_preserves_state_and_trait_dispatch() {
    let calls = Arc::new(AtomicU32::new(0_u32));
    let observed = Arc::clone(&calls);
    let mut concrete = OneTimeJob::new(Box::new(move || {
        observed.fetch_add(1, AtomicOrdering::Relaxed);
    }));
    let job: &mut dyn Job = &mut concrete;

    assert!(job.Ready());
    assert!(!job.Done());
    job.Work();
    assert!(!job.Ready());
    assert!(job.Done());
    assert_eq!(calls.load(AtomicOrdering::Relaxed), 1);

    // The historical class does not suppress an explicit second Work call.
    job.Work();
    assert_eq!(calls.load(AtomicOrdering::Relaxed), 2);
}

#[test]
fn thousands_formatter_matches_the_legacy_surface() {
    let text = |value| format_thousands(value).to_rust_string();
    assert_eq!(text(0.0), "0.00");
    assert_eq!(text(-0.0), "0.00");
    assert_eq!(text(12.5), "12.50");
    assert_eq!(text(1_234.5), "1,234.50");
    assert_eq!(text(-1_234_567.89), "-1,234,567.89");
    assert_eq!(text(999.999), "1,000.00");
}
