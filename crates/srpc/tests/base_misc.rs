#![allow(non_snake_case)]

#[path = "../src/base/misc.rs"]
mod base_misc;

use base_misc::{format_thousands, Job, OneTimeJob};
use std::cell::Cell;
use std::rc::Rc;

#[test]
fn one_time_job_preserves_ready_work_done_state_and_fn_mut() {
    let calls = Rc::new(Cell::new(0_u32));
    let observed = calls.clone();
    let mut job = OneTimeJob::new(Box::new(move || {
        observed.set(observed.get() + 1);
    }));

    assert!(job.Ready());
    assert!(!job.Done());

    job.Work();
    assert!(!job.Ready());
    assert!(job.Done());
    assert_eq!(calls.get(), 1);

    // The legacy type does not suppress an explicit second Work call.
    job.Work();
    assert_eq!(calls.get(), 2);
}

#[test]
fn one_time_job_dispatches_through_the_job_trait() {
    let calls = Rc::new(Cell::new(0_u32));
    let observed = calls.clone();
    let mut concrete = OneTimeJob::new(Box::new(move || {
        observed.set(observed.get() + 1);
    }));
    let job: &mut dyn Job = &mut concrete;

    assert!(job.Ready());
    job.Work();
    assert!(job.Done());
    assert_eq!(calls.get(), 1);
}

#[test]
fn thousands_formatter_matches_the_legacy_surface() {
    assert_eq!(format_thousands(0.0), "0.00");
    assert_eq!(format_thousands(-0.0), "0.00");
    assert_eq!(format_thousands(12.5), "12.50");
    assert_eq!(format_thousands(1_234.5), "1,234.50");
    assert_eq!(format_thousands(-1_234_567.89), "-1,234,567.89");
    assert_eq!(format_thousands(999.999), "1,000.00");
}

#[test]
fn dead_unrepresentable_helpers_stay_removed() {
    let owner = include_str!("../src/base/misc.rs");
    assert!(!owner.contains("pub fn clamp"));
    assert!(!owner.contains("pub fn get_ncpu"));
}
