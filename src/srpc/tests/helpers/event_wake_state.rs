//! Crate-internal checks of the event wake state's deadline map (S4 step 3 of
//! docs/dev/lion-runtime-plan.md) that the public surface cannot see: the
//! private next-deadline accessor a later driver (S3) will sleep on.
//! Behaviour through the public API is in tests/reactor_deadline_rust.rs.

use super::*;

fn run_until(reactor: &Reactor, done: impl Fn() -> bool) {
    let limit = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !done() {
        assert!(std::time::Instant::now() < limit);
        std::thread::sleep(std::time::Duration::from_millis(1));
        reactor.run_loop(false, true);
    }
}

#[test]
fn next_deadline_is_the_earliest_pending_entry_of_the_owner() {
    let reactor = Reactor::get_reactor();
    let before: Option<u64> = event_next_deadline_us::<()>(&reactor);

    let late = create_sp_timeout_event(60_000_000);
    let late_deadline = late.wakeup_time_ + 1;
    let expected_late = match before {
        Some(b) => b.min(late_deadline),
        None => late_deadline,
    };
    assert_eq!(event_next_deadline_us::<()>(&reactor), Some(expected_late));

    let early = create_sp_timeout_event(3_000);
    let early_deadline = early.wakeup_time_ + 1;
    assert!(early_deadline < late_deadline);
    assert_eq!(event_next_deadline_us::<()>(&reactor), Some(early_deadline.min(expected_late)));

    // Only the reactor that owns this thread's wake state answers.
    let disk = Reactor::get_disk_reactor();
    assert_eq!(event_next_deadline_us::<()>(&disk), None);

    // Once the early deadline is served, the next one is the late timer.
    run_until(&reactor, || early.status() == EventStatus::DONE);
    assert_eq!(event_next_deadline_us::<()>(&reactor), Some(expected_late));
}

#[test]
fn next_deadline_is_a_lower_bound_under_lazy_deletion() {
    // A wait that ends first leaves its entry, and the accessor still reports
    // it until it is popped; a driver waking then finds nothing to serve and
    // asks again.
    let reactor = Reactor::get_reactor();
    assert_eq!(event_next_deadline_us::<()>(&reactor), None);
    let ev = create_sp_int_event(1);
    let resumed = std::rc::Rc::new(Cell::new(0usize));
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(200_000);
            r.set(r.get() + 1);
        });
    }
    let deadline = ev.wakeup_time();
    assert_eq!(event_next_deadline_us::<()>(&reactor), Some(deadline));
    ev.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(event_next_deadline_us::<()>(&reactor), Some(deadline));
    run_until(&reactor, || event_next_deadline_us::<()>(&reactor).is_none());
    assert_eq!(resumed.get(), 1);
    assert_eq!(ev.status(), EventStatus::DONE);
}
