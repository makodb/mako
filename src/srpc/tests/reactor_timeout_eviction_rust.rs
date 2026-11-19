// run_loop evicts timed-out events from its waiting and composite queues, and
// the event keeps its TIMEOUT status after it has been evicted.
//
// Before S4 step 0 (docs/dev/lion-runtime-plan.md), run_loop's retain removed
// only DONE entries. Every wait that timed out therefore stayed queued, and
// each pass re-tested it for as long as the reactor lived. A TIMEOUT event can
// never change state again, because event_test_impl only moves WAIT to READY.
// The re-test did nothing except cost time and keep the event alive.
//
// Every #[test] runs on its own thread, so each one gets a fresh thread-local
// Reactor. The baselines are still measured rather than assumed.

use srpc::reactor::{
    create_sp_int_event, create_sp_never_event, create_sp_waitall_from, create_sp_waitany,
    event_wake_report, EventPollable, EventStatus, Fiber, Reactor,
};
use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

// Long enough that creating every waiter finishes well before the first
// deadline, even under a sanitizer. The "all parked" assertions depend on it.
const WAIT_US: u64 = 50_000;

// (waiting_events_, composite_events_, live deadlines)
//
// S4 step 3 moved timed waits from the linear timeout_events_ queue to the
// reactor's deadline map, whose entries are deleted lazily. The live count
// excludes entries whose wait has already ended, so it moves exactly as
// timeout_events_ used to.
fn queue_lens(reactor: &Reactor) -> (usize, usize, usize) {
    (
        reactor.waiting_events_.borrow().len(),
        reactor.composite_events_.borrow().len(),
        event_wake_report::<()>().live_deadlines,
    )
}

// Drive the owner reactor until `resumed` reaches `expected`. Only the
// reactor's timer check can resume these fibers, because nothing sets their
// events.
fn drive_until_resumed(reactor: &Reactor, resumed: &Cell<usize>, expected: usize) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while resumed.get() < expected {
        assert!(
            Instant::now() < deadline,
            "only {} of {} timed-out waits were resumed",
            resumed.get(),
            expected
        );
        std::thread::sleep(Duration::from_millis(1));
        reactor.run_loop(false, true);
    }
}

#[test]
fn timed_out_waits_leave_the_waiting_and_composite_queues() {
    const N: usize = 16;
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);

    let resumed = Rc::new(Cell::new(0usize));
    let mut held: Vec<Arc<dyn EventPollable>> = Vec::new();
    for _ in 0..N {
        // Neither event can become ready, so both waits end in TIMEOUT.
        let leaf = create_sp_never_event();
        let composite = create_sp_waitany(create_sp_never_event(), create_sp_int_event(1));
        held.push(leaf.clone());
        held.push(composite.clone());

        let leaf_resumed = resumed.clone();
        Fiber::create_run(move || {
            leaf.wait_timeout(WAIT_US);
            leaf_resumed.set(leaf_resumed.get() + 1);
        });
        let composite_resumed = resumed.clone();
        Fiber::create_run(move || {
            composite.wait_timeout(WAIT_US);
            composite_resumed.set(composite_resumed.get() + 1);
        });
    }

    // Every wait is parked, and each has a live deadline. Since S4 steps 3
    // and 4 neither a NeverEvent nor a WaitAny joins a scanned queue: nothing
    // can make the leaf ready, and the composite's children tell it when they
    // become ready. Without this check, returning to baseline below would
    // prove nothing.
    assert_eq!(resumed.get(), 0, "a wait finished before its deadline");
    assert_eq!(
        queue_lens(&reactor),
        (baseline.0, baseline.1, baseline.2 + 2 * N)
    );

    drive_until_resumed(&reactor, &resumed, 2 * N);
    for ev in &held {
        assert_eq!(ev.status(), EventStatus::TIMEOUT);
    }

    // More passes change nothing. Without eviction, these entries stay queued
    // no matter how many passes run.
    reactor.run_loop(false, true);
    reactor.run_loop(false, true);
    assert_eq!(
        queue_lens(&reactor),
        baseline,
        "timed-out events must leave (waiting, composite, timeout)"
    );
    // TIMEOUT is sticky: eviction removes the queue entry, not the status.
    for ev in &held {
        assert_eq!(ev.status(), EventStatus::TIMEOUT);
    }
}

#[test]
fn run_loop_stops_testing_an_event_once_it_has_timed_out() {
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);

    // Each predicate counts its calls and is never satisfied. The leaf runs
    // its predicate whenever the leaf is tested. The composite's child runs
    // its predicate whenever the WaitAll is tested.
    let leaf_probes = Rc::new(Cell::new(0usize));
    let leaf = create_sp_int_event(1);
    let leaf_counter = leaf_probes.clone();
    *leaf.state_.test_.borrow_mut() = Some(Box::new(move |_value: i32| -> bool {
        leaf_counter.set(leaf_counter.get() + 1);
        false
    }));

    let child_probes = Rc::new(Cell::new(0usize));
    let child = create_sp_int_event(1);
    let child_counter = child_probes.clone();
    *child.state_.test_.borrow_mut() = Some(Box::new(move |_value: i32| -> bool {
        child_counter.set(child_counter.get() + 1);
        false
    }));
    let children: Vec<Arc<dyn EventPollable>> = vec![child.clone()];
    let composite = create_sp_waitall_from(&children);

    let resumed = Rc::new(Cell::new(0usize));
    let waiter_leaf = leaf.clone();
    let leaf_resumed = resumed.clone();
    Fiber::create_run(move || {
        waiter_leaf.wait_timeout(WAIT_US);
        leaf_resumed.set(leaf_resumed.get() + 1);
    });
    let waiter_composite = composite.clone();
    let composite_resumed = resumed.clone();
    Fiber::create_run(move || {
        waiter_composite.wait_timeout(WAIT_US);
        composite_resumed.set(composite_resumed.get() + 1);
    });
    drive_until_resumed(&reactor, &resumed, 2);
    assert_eq!(leaf.status(), EventStatus::TIMEOUT);
    assert_eq!(composite.status(), EventStatus::TIMEOUT);

    // Let the eviction pass run, then count from a quiet reactor. Before
    // eviction, every pass ran the leaf predicate once (waiting queue) and the
    // child predicate twice (waiting and composite queues).
    reactor.run_loop(false, true);
    let leaf_before = leaf_probes.get();
    let child_before = child_probes.get();
    const PASSES: usize = 64;
    for _ in 0..PASSES {
        reactor.run_loop(false, true);
    }
    assert_eq!(leaf_probes.get(), leaf_before, "a TIMEOUT leaf was tested again");
    assert_eq!(child_probes.get(), child_before, "a TIMEOUT composite was tested again");
    assert_eq!(queue_lens(&reactor), baseline);
    assert_eq!(leaf.status(), EventStatus::TIMEOUT);
    assert_eq!(composite.status(), EventStatus::TIMEOUT);
}

#[test]
fn a_late_set_neither_revives_a_timed_out_event_nor_requeues_it() {
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);

    let ev = create_sp_int_event(1);
    let resumed = Rc::new(Cell::new(0usize));
    let waiter = ev.clone();
    let waiter_resumed = resumed.clone();
    Fiber::create_run(move || {
        waiter.wait_timeout(WAIT_US);
        waiter_resumed.set(waiter_resumed.get() + 1);
    });
    drive_until_resumed(&reactor, &resumed, 1);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);

    // The value now meets the target, so the event is ready. It still stays
    // TIMEOUT, because set() tests the event and that test cannot move a
    // TIMEOUT event. This is the status test_timeout_race.cc's
    // EventStatusAfterTimeout reads from a second fiber.
    ev.set(1);
    assert!(ev.is_ready());
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    reactor.run_loop(false, true);
    reactor.run_loop(false, true);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    assert_eq!(queue_lens(&reactor), baseline);
    assert_eq!(resumed.get(), 1, "the timed-out waiter must not be resumed again");
}
