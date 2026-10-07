// Timers go through a per-reactor deadline map (S4 step 3 of
// docs/dev/lion-runtime-plan.md). A timed wait, and every TimeoutEvent from its
// creation, has an entry keyed by deadline, and check_timeout serves the
// expired prefix in deadline order. It replaced a linear scan of every timed
// wait on every pass, plus a clock read in every TimeoutEvent test.
//
// The rule at a timed wait's deadline is unchanged: READY if the event is
// ready, else TIMEOUT, and TIMEOUT is sticky. Entries are deleted lazily: a
// wait that ends first leaves its entry, which is dropped unserved later.
//
// Every #[test] runs on its own thread, so each one gets a fresh thread-local
// Reactor. Baselines are still measured rather than assumed. Durations are
// hundreds of milliseconds where a test must act before a deadline: the gate
// runs on a shared machine, and a thread can lose tens of milliseconds.

use srpc::basetypes::Time;
use srpc::reactor::{
    create_sp_int_event, create_sp_never_event, create_sp_timeout_event, event_wake_report,
    EventPollable, EventStatus, Fiber, Reactor,
};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};

fn counter() -> Rc<Cell<usize>> {
    Rc::new(Cell::new(0usize))
}

// Drive the owner reactor until `done()` holds. Only the reactor's deadline
// check can make these waits progress.
fn drive_until(reactor: &Reactor, what: &str, done: impl Fn() -> bool) {
    let limit = Instant::now() + Duration::from_secs(5);
    while !done() {
        assert!(Instant::now() < limit, "timed out waiting for: {what}");
        std::thread::sleep(Duration::from_millis(1));
        reactor.run_loop(false, true);
    }
}

#[test]
fn expired_deadlines_resume_in_deadline_order() {
    // Three timers that all expire before one pass, created in the order of
    // their labels but with deadlines 300, 100 and 200 ms out. They resume in
    // deadline order. The old scan resumed the TimeoutEvent first (its own
    // per-pass test found it) and the rest in the order they were waited.
    let reactor = Reactor::get_reactor();
    let order = Rc::new(RefCell::new(Vec::<&'static str>::new()));

    let sleeper = create_sp_timeout_event(300_000);
    {
        let (ev, log) = (sleeper.clone(), order.clone());
        Fiber::create_run(move || {
            ev.wait();
            log.borrow_mut().push("timeout-event-300ms");
        });
    }
    let int_ev = create_sp_int_event(1);
    {
        let (ev, log) = (int_ev.clone(), order.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(100_000);
            log.borrow_mut().push("int-100ms");
        });
    }
    let never = create_sp_never_event();
    {
        let (ev, log) = (never.clone(), order.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(200_000);
            log.borrow_mut().push("never-200ms");
        });
    }
    assert!(order.borrow().is_empty());

    std::thread::sleep(Duration::from_millis(350));
    reactor.run_loop(false, true);
    assert_eq!(*order.borrow(), ["int-100ms", "never-200ms", "timeout-event-300ms"]);
    assert_eq!(int_ev.status(), EventStatus::TIMEOUT);
    assert_eq!(never.status(), EventStatus::TIMEOUT);
    // A TimeoutEvent's own deadline makes it ready: it resumes READY->DONE.
    assert_eq!(sleeper.status(), EventStatus::DONE);
}

#[test]
fn timers_join_no_scanned_queue() {
    // The per-pass scan no longer visits timed waits, TimeoutEvents or
    // NeverEvents: each has a deadline entry instead.
    let reactor = Reactor::get_reactor();
    let waiting = reactor.waiting_events_.borrow().len();
    let composite = reactor.composite_events_.borrow().len();
    let base = event_wake_report::<()>();

    let resumed = counter();
    let t = create_sp_timeout_event(60_000_000);
    {
        let (ev, r) = (t.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    let n = create_sp_never_event();
    {
        let (ev, r) = (n.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(60_000_000);
            r.set(r.get() + 1);
        });
    }
    let i = create_sp_int_event(1);
    {
        let (ev, r) = (i.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(60_000_000);
            r.set(r.get() + 1);
        });
    }
    assert_eq!(resumed.get(), 0);
    assert_eq!(reactor.waiting_events_.borrow().len(), waiting);
    assert_eq!(reactor.composite_events_.borrow().len(), composite);
    // The TimeoutEvent has two entries: its own deadline from creation, and
    // its wait's; the NeverEvent and the IntEvent one each.
    assert_eq!(event_wake_report::<()>().live_deadlines, base.live_deadlines + 4);
    i.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
}

#[test]
fn a_wait_that_ends_first_leaves_its_entry_to_be_dropped_unserved() {
    let reactor = Reactor::get_reactor();
    let base = event_wake_report::<()>();
    let ev = create_sp_int_event(1);
    let resumed = counter();
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(200_000);
            r.set(r.get() + 1);
        });
    }
    let parked = event_wake_report::<()>();
    assert_eq!(parked.live_deadlines, base.live_deadlines + 1);
    assert_eq!(parked.deadline_entries, base.deadline_entries + 1);

    ev.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(ev.status(), EventStatus::DONE);
    // The entry is still there, but it is no longer live.
    let ended = event_wake_report::<()>();
    assert_eq!(ended.live_deadlines, base.live_deadlines);
    assert_eq!(ended.deadline_entries, base.deadline_entries + 1);

    // At the deadline it is popped and dropped without effect.
    std::thread::sleep(Duration::from_millis(210));
    reactor.run_loop(false, true);
    reactor.run_loop(false, true);
    assert_eq!(event_wake_report::<()>().deadline_entries, base.deadline_entries);
    assert_eq!(resumed.get(), 1, "a finished wait was resumed again at its old deadline");
    assert_eq!(ev.status(), EventStatus::DONE);
}

#[test]
fn a_stale_entry_never_times_out_a_later_wait_of_the_same_event() {
    // The event's first wait is timed and ends early. The event is re-armed
    // and waited again, first untimed, then with a later deadline. The first
    // wait's entry is still in the map at its old deadline and must not end
    // either later wait.
    let reactor = Reactor::get_reactor();
    let ev = create_sp_int_event(1);
    let phase = counter();
    let statuses = Rc::new(RefCell::new(Vec::<EventStatus>::new()));
    {
        let (e, p, log) = (ev.clone(), phase.clone(), statuses.clone());
        Fiber::create_run(move || {
            e.wait_timeout(100_000);
            log.borrow_mut().push(e.status());
            p.set(1);
            // Re-arm: not ready any more, so a test() moves DONE -> INIT.
            e.value_.set(0);
            assert!(!e.test());
            assert_eq!(e.status(), EventStatus::INIT);
            e.wait();
            log.borrow_mut().push(e.status());
            p.set(2);
            e.value_.set(0);
            assert!(!e.test());
            e.wait_timeout(100_000);
            log.borrow_mut().push(e.status());
            p.set(3);
        });
    }
    ev.set(1);
    reactor.run_loop(false, true);
    assert_eq!(phase.get(), 1);

    // Past the first wait's deadline: the untimed wait is still parked.
    std::thread::sleep(Duration::from_millis(110));
    reactor.run_loop(false, true);
    reactor.run_loop(false, true);
    assert_eq!(phase.get(), 1, "a stale deadline ended an untimed wait");
    assert_eq!(ev.status(), EventStatus::WAIT);

    ev.set(1);
    reactor.run_loop(false, true);
    // 3 only if this thread stalled past the third wait's deadline.
    assert!(phase.get() >= 2);
    let third_deadline = ev.wakeup_time();
    drive_until(&reactor, "the third wait's own deadline", || phase.get() == 3);
    assert!(Time::now(true) >= third_deadline);
    assert_eq!(
        *statuses.borrow(),
        [EventStatus::DONE, EventStatus::DONE, EventStatus::TIMEOUT]
    );
}

#[test]
fn a_timed_wait_made_ready_without_an_owner_test_completes_at_its_deadline() {
    // Two timed waits whose events become ready without an owner-thread
    // test(). One has its value written directly. The other is also marked
    // READY, the state a foreign-thread set() leaves: it takes the WAIT->READY
    // edge but queues nothing, because only the owner touches the ready
    // queue. Rust cannot call set() on another thread (events are !Send), so
    // the test writes that state on the owner.
    //
    // Neither is noticed by a pass before the deadline. At the deadline the
    // rule "READY if ready, else TIMEOUT" resumes both as ready. Before S4
    // step 3, check_timeout also took READY entries on every pass, so the
    // second waiter resumed on the first pass after the write.
    let reactor = Reactor::get_reactor();
    let written = create_sp_int_event(1);
    let marked = create_sp_int_event(1);
    let resumed = counter();
    // Each waiter counts itself if it resumed before its own deadline.
    let early = counter();
    for ev in [written.clone(), marked.clone()] {
        let (r, e) = (resumed.clone(), early.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(300_000);
            assert_eq!(ev.status(), EventStatus::DONE);
            if Time::now(true) < ev.wakeup_time() {
                e.set(e.get() + 1);
            }
            r.set(r.get() + 1);
        });
    }
    assert!(written.wakeup_time() > 0 && marked.wakeup_time() > 0);

    written.value_.set(1);
    marked.value_.set(1);
    marked.set_status(EventStatus::READY);
    assert_eq!(written.status(), EventStatus::WAIT);

    drive_until(&reactor, "both deadlines", || resumed.get() == 2);
    assert_eq!(early.get(), 0, "a waiter resumed before its deadline");
    assert_eq!(written.status(), EventStatus::DONE);
    assert_eq!(marked.status(), EventStatus::DONE);
}

#[test]
fn nothing_is_resumed_before_its_deadline() {
    // Per-pass complement of the test above: every pass that resumes the
    // waiter must have run at or after the deadline.
    let reactor = Reactor::get_reactor();
    let ev = create_sp_int_event(1);
    let resumed = counter();
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(20_000);
            r.set(r.get() + 1);
        });
    }
    let deadline = ev.wakeup_time();
    ev.value_.set(1);
    ev.set_status(EventStatus::READY);
    let limit = Instant::now() + Duration::from_secs(5);
    while resumed.get() == 0 {
        assert!(Instant::now() < limit);
        reactor.run_loop(false, true);
        if resumed.get() > 0 {
            assert!(Time::now(true) >= deadline, "resumed before the deadline");
        }
        std::thread::sleep(Duration::from_micros(200));
    }
    assert_eq!(ev.status(), EventStatus::DONE);
}

#[test]
fn a_timeout_event_fires_at_its_creation_deadline_even_when_waited_late() {
    // A TimeoutEvent is ready once its creation time plus its duration has
    // passed, however late the wait starts. Its own deadline entry, registered
    // at creation, resumes the waiter then, not a full duration after the wait
    // began.
    let reactor = Reactor::get_reactor();
    let t = create_sp_timeout_event(300_000);
    std::thread::sleep(Duration::from_millis(200));
    let wait_started = Time::now(true);
    let resumed_at = Rc::new(Cell::new(0u64));
    {
        let (ev, at) = (t.clone(), resumed_at.clone());
        Fiber::create_run(move || {
            ev.wait();
            at.set(Time::now(true));
        });
    }
    drive_until(&reactor, "the TimeoutEvent", || resumed_at.get() != 0);
    assert!(resumed_at.get() > t.wakeup_time_);
    assert!(
        resumed_at.get() < wait_started + 300_000,
        "resumed a full duration after the wait, not at the creation deadline"
    );
    assert_eq!(t.status(), EventStatus::DONE);
}

#[test]
fn an_unwaited_timeout_event_becomes_done_at_its_deadline() {
    // The deadline map tests a TimeoutEvent whether or not anyone waits on
    // it, so a composite parent can learn of it (S4 step 4). Unwaited, the
    // test moves it INIT -> DONE; a later wait returns at once.
    let reactor = Reactor::get_reactor();
    let t = create_sp_timeout_event(5_000);
    assert_eq!(t.status(), EventStatus::INIT);
    drive_until(&reactor, "the unwaited TimeoutEvent", || t.status() == EventStatus::DONE);
    let done = counter();
    {
        let (ev, d) = (t.clone(), done.clone());
        Fiber::create_run(move || {
            ev.wait();
            d.set(1);
        });
    }
    assert_eq!(done.get(), 1, "a wait on a fired TimeoutEvent did not return at once");
}

#[test]
fn early_ending_waits_do_not_accumulate_deadline_entries() {
    // Lazy deletion with a sweep: a long timeout on a wait that ends at once
    // must not keep a map entry until the timeout.
    const WAITS: usize = 1000;
    let reactor = Reactor::get_reactor();
    let base = event_wake_report::<()>();
    let resumed = counter();
    for _ in 0..WAITS {
        let ev = create_sp_int_event(1);
        {
            let (e, r) = (ev.clone(), resumed.clone());
            Fiber::create_run(move || {
                e.wait_timeout(60_000_000);
                r.set(r.get() + 1);
            });
        }
        ev.set(1);
        reactor.run_loop(false, true);
    }
    assert_eq!(resumed.get(), WAITS);
    let after = event_wake_report::<()>();
    assert_eq!(after.live_deadlines, base.live_deadlines);
    assert!(
        after.deadline_entries <= base.deadline_entries + 2 * base.live_deadlines + 65,
        "{} stale deadline entries were kept",
        after.deadline_entries
    );
}

#[test]
fn timeout_is_sticky_after_the_deadline_rule() {
    let reactor = Reactor::get_reactor();
    let base = event_wake_report::<()>();
    let ev = create_sp_int_event(1);
    let resumed = counter();
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(5_000);
            r.set(r.get() + 1);
        });
    }
    drive_until(&reactor, "the timeout", || resumed.get() == 1);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    ev.set(1);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    reactor.run_loop(false, true);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    assert_eq!(resumed.get(), 1);
    assert_eq!(event_wake_report::<()>().deadline_entries, base.deadline_entries);
}

#[test]
fn a_waiter_that_waits_again_at_once_is_not_resumed_by_a_second_entry() {
    // The event is set after its deadline has passed but before any pass, so
    // one pass finds it twice: through the ready queue, and through its
    // deadline, which hands a READY event over. The first dispatch resumes
    // the waiter, which re-arms the event and waits on it again at once. The
    // second entry now finds a waiting event and must leave it alone. Before
    // S4 step 3 this same pass order tripped the dispatch's TIMEOUT check,
    // because check_timeout took READY entries on every pass.
    let reactor = Reactor::get_reactor();
    let ev = create_sp_int_event(1);
    let phase = counter();
    {
        let (e, p) = (ev.clone(), phase.clone());
        Fiber::create_run(move || {
            e.wait_timeout(5_000);
            assert_eq!(e.status(), EventStatus::DONE);
            p.set(1);
            e.value_.set(0);
            assert!(!e.test());
            e.wait();
            assert_eq!(e.status(), EventStatus::DONE);
            p.set(2);
        });
    }
    std::thread::sleep(Duration::from_millis(10));
    ev.set(1);
    reactor.run_loop(false, true);
    assert_eq!(phase.get(), 1, "the second wait was resumed by the first wait's entry");
    assert_eq!(ev.status(), EventStatus::WAIT);
    ev.set(1);
    reactor.run_loop(false, true);
    assert_eq!(phase.get(), 2);
}
