// Events that change only through their own methods wake on change: their
// WAIT->READY edge queues them on the owner's ready queue, and run_loop drains
// that queue instead of re-testing them on every pass. The waiter still
// resumes only when the owner drains; set(), vote_*() and test() never resume
// it inline.
//
// This is S4 steps 1-2 of docs/dev/lion-runtime-plan.md. The converted events
// are BoxEvent, IntEvent without a predicate (which covers SharedIntEvent's
// wait_until_gte and QuorumEvent's finalize_event_), and QuorumEvent.
// Timers moved to a deadline map in step 3 (tests/reactor_deadline_rust.rs),
// composites to parent links in step 4 (tests/reactor_composite_rust.rs), and
// predicate IntEvents to pings in step 5 (tests/reactor_ping_rust.rs). No
// event waited on its owner thread is on the per-pass scan any more.
//
// Every #[test] runs on its own thread, so each one gets a fresh thread-local
// Reactor. Queue baselines are still measured rather than assumed.

use srpc::reactor::{
    create_sp_box_event, create_sp_int_event, create_sp_never_event, create_sp_quorum_event,
    create_sp_waitany, event_wake_report, EventPollable, EventStatus, Fiber, QuorumEventWrapper,
    Reactor, SharedIntEvent,
};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

// (waiting_events_, composite_events_, live deadlines)
//
// S4 step 3 moved timed waits from timeout_events_ to the reactor's deadline
// map. Its entries are deleted lazily; the live count excludes entries whose
// wait has already ended, so it moves exactly as timeout_events_ used to.
fn queue_lens(reactor: &Reactor) -> (usize, usize, usize) {
    (
        reactor.waiting_events_.borrow().len(),
        reactor.composite_events_.borrow().len(),
        event_wake_report::<()>().live_deadlines,
    )
}

// Far beyond any test's runtime, so a timed wait here never times out; it only
// gives the event a live deadline.
const LONG_WAIT_US: u64 = 60_000_000;

// Passes run while nothing sets the event. Under the old scan, the first one
// already found a directly written value.
const IDLE_PASSES: usize = 64;

fn counter() -> Rc<Cell<usize>> {
    Rc::new(Cell::new(0usize))
}

#[test]
fn self_notifying_waits_join_no_scanned_queue() {
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);

    let resumed = counter();

    let int_ev = create_sp_int_event(1);
    let box_ev = create_sp_box_event::<i32>();
    let quorum = create_sp_quorum_event(3, 2);
    let timed_int = create_sp_int_event(1);
    let shared = Rc::new(RefCell::new(SharedIntEvent { value_: 0, events_: Vec::new() }));

    {
        let (ev, r) = (int_ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    {
        let (ev, r) = (box_ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    {
        let (ev, r) = (quorum.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    {
        let (ev, r) = (timed_int.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait_timeout(LONG_WAIT_US);
            r.set(r.get() + 1);
        });
    }
    // SharedIntEvent has no users, and its wait_until_gte holds `&mut self`
    // across the park, so safe Rust cannot call set() while it waits. Build
    // the waiter the way wait_until_gte does (a target IntEvent pushed onto
    // events_) and let SharedIntEvent::set reach it.
    let shared_ev = create_sp_int_event(5);
    shared.borrow_mut().events_.push(shared_ev.clone());
    {
        let (ev, r) = (shared_ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    assert_eq!(resumed.get(), 0, "a wait finished before its event was set");

    // Five parked waits, and none of them is on a scanned queue. Only the
    // timed one has a deadline, which is how it can still time out.
    assert_eq!(queue_lens(&reactor), (baseline.0, baseline.1, baseline.2 + 1));

    // The classes S4 converted later join no scanned queue either: a
    // NeverEvent since step 3 (nothing can make it ready, so its timed wait
    // only has a deadline), a WaitAny since step 4 (its children tell it),
    // and a predicate IntEvent since step 5 (its publisher sets or pings it).
    let predicate_ev = create_sp_int_event(1);
    *predicate_ev.state_.test_.borrow_mut() = Some(Box::new(|_value: i32| -> bool { false }));
    let never = create_sp_never_event();
    let any = create_sp_waitany(create_sp_never_event(), create_sp_never_event());
    // These three park for the rest of the test; nothing ever sets them.
    {
        let ev = predicate_ev.clone();
        Fiber::create_run(move || ev.wait());
    }
    {
        let ev = never.clone();
        Fiber::create_run(move || ev.wait_timeout(LONG_WAIT_US));
    }
    {
        let ev = any.clone();
        Fiber::create_run(move || ev.wait());
    }
    assert_eq!(
        queue_lens(&reactor),
        (baseline.0, baseline.1, baseline.2 + 2),
        "an event waited on its owner thread joined a scanned queue"
    );

    // Each converted wait still resumes through the ready queue.
    int_ev.set(1);
    box_ev.set(&7);
    quorum.vote_yes();
    quorum.vote_yes();
    timed_int.set(1);
    shared.borrow_mut().set(&5);
    assert_eq!(resumed.get(), 0, "set() or vote_yes() resumed a waiter inline");
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 5);
    for ev in [
        int_ev.clone() as Arc<dyn EventPollable>,
        box_ev.clone() as Arc<dyn EventPollable>,
        quorum.clone() as Arc<dyn EventPollable>,
        timed_int.clone() as Arc<dyn EventPollable>,
        shared_ev.clone() as Arc<dyn EventPollable>,
    ] {
        assert_eq!(ev.status(), EventStatus::DONE);
    }
    // The timed event's deadline stopped being live when it was dispatched.
    assert_eq!(queue_lens(&reactor), (baseline.0, baseline.1, baseline.2 + 1));
}

#[test]
fn run_loop_does_not_retest_a_self_notifying_event() {
    // Each event is made ready by writing its state directly, which does not
    // test it. The old per-pass scan found such a write on its next pass; a
    // wake-on-change event is only noticed when something tests it. test()
    // itself then queues it, and the next drain resumes the waiter.
    let reactor = Reactor::get_reactor();
    let resumed = counter();

    let int_ev = create_sp_int_event(1);
    let box_ev = create_sp_box_event::<i32>();
    {
        let (ev, r) = (int_ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }
    {
        let (ev, r) = (box_ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            ev.wait();
            r.set(r.get() + 1);
        });
    }

    int_ev.value_.set(1);
    box_ev.is_set_.set(true);
    assert!(int_ev.is_ready() && box_ev.is_ready());
    for _ in 0..IDLE_PASSES {
        reactor.run_loop(false, true);
    }
    assert_eq!(resumed.get(), 0, "run_loop re-tested a wake-on-change event");
    assert_eq!(int_ev.status(), EventStatus::WAIT);
    assert_eq!(box_ev.status(), EventStatus::WAIT);

    assert!(int_ev.test());
    assert!(box_ev.test());
    assert_eq!(int_ev.status(), EventStatus::READY);
    assert_eq!(box_ev.status(), EventStatus::READY);
    assert_eq!(resumed.get(), 0, "test() resumed a waiter inline");
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 2);
    assert_eq!(int_ev.status(), EventStatus::DONE);
    assert_eq!(box_ev.status(), EventStatus::DONE);
}

#[test]
fn quorum_counter_write_then_test_wakes_the_waiter() {
    // Mako's paxos FeedResponse increments n_voted_yes_ itself and then calls
    // test() (paxos/commo.h). That test() must reach the ready queue.
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);
    let resumed = counter();
    let wrapper = Rc::new(QuorumEventWrapper::new(3, 2));
    {
        let (w, r) = (wrapper.clone(), resumed.clone());
        Fiber::create_run(move || {
            w.wait();
            r.set(r.get() + 1);
        });
    }
    let q = wrapper.q();
    q.n_voted_yes_.set(q.n_voted_yes_.get() + 1);
    assert!(!wrapper.test());
    q.n_voted_yes_.set(q.n_voted_yes_.get() + 1);
    for _ in 0..IDLE_PASSES {
        reactor.run_loop(false, true);
    }
    assert_eq!(resumed.get(), 0, "run_loop re-tested a quorum");
    // A quorum is no longer a composite, so it joins neither scanned queue.
    assert_eq!(queue_lens(&reactor), baseline);
    assert!(!wrapper.q().is_composite_event());

    assert!(wrapper.test());
    assert_eq!(q.status(), EventStatus::READY);
    assert_eq!(resumed.get(), 0, "test() resumed the quorum waiter inline");
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(q.status(), EventStatus::DONE);
    assert!(q.yes());
}

#[test]
fn set_and_vote_inside_a_fiber_defer_the_waiter_to_the_drain() {
    // Mako's raft quorum code writes state after voting, and the waiter reads
    // it (raft/commo.h, server.cc). A waiter that resumed inside vote_yes()
    // or set() would miss that write.
    let reactor = Reactor::get_reactor();
    let quorum = create_sp_quorum_event(3, 2);
    let int_ev = create_sp_int_event(1);
    let after_vote = Rc::new(Cell::new(0i32));
    let after_set = Rc::new(Cell::new(0i32));
    let seen = Rc::new(RefCell::new(Vec::<(&'static str, i32)>::new()));

    {
        let (q, state, log) = (quorum.clone(), after_vote.clone(), seen.clone());
        Fiber::create_run(move || {
            q.wait();
            log.borrow_mut().push(("quorum", state.get()));
        });
    }
    {
        let (ev, state, log) = (int_ev.clone(), after_set.clone(), seen.clone());
        Fiber::create_run(move || {
            ev.wait();
            log.borrow_mut().push(("int", state.get()));
        });
    }

    let (q, ev) = (quorum.clone(), int_ev.clone());
    let (vote_state, set_state, log) = (after_vote.clone(), after_set.clone(), seen.clone());
    let setter_ran = counter();
    let setter_flag = setter_ran.clone();
    // create_run runs the setter synchronously, then drains with its built-in
    // run_loop(false, true).
    Fiber::create_run(move || {
        q.vote_yes();
        q.vote_yes();
        assert!(log.borrow().is_empty(), "vote_yes() resumed the quorum waiter inline");
        vote_state.set(42);
        ev.set(1);
        assert!(log.borrow().is_empty(), "set() resumed the waiter inline");
        set_state.set(7);
        setter_flag.set(1);
    });
    assert_eq!(setter_ran.get(), 1);
    // Both waiters ran in create_run's drain, after the setter finished, so
    // each saw the state written after its event became ready.
    let mut got = seen.borrow().clone();
    got.sort();
    assert_eq!(got, vec![("int", 7), ("quorum", 42)]);
    assert_eq!(quorum.status(), EventStatus::DONE);
    assert_eq!(int_ev.status(), EventStatus::DONE);
    drop(reactor);
}

#[test]
fn set_outside_any_fiber_waits_for_the_next_drain() {
    let reactor = Reactor::get_reactor();
    let resumed = counter();
    let ev = create_sp_int_event(1);
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait();
            r.set(r.get() + 1);
        });
    }
    ev.set(1);
    assert_eq!(ev.status(), EventStatus::READY);
    assert_eq!(resumed.get(), 0, "set() outside a fiber resumed the waiter inline");
    // A second set is not a second edge: the event is queued once.
    ev.set(2);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(ev.status(), EventStatus::DONE);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1, "a queued event resumed its waiter twice");
}

#[test]
fn one_run_loop_drains_a_chain_of_wake_on_change_events() {
    // test_reactor_extended.cc's EventChain, across all three converted
    // classes: each resumed fiber makes the next event ready, and one
    // run_loop(false, true) call serves the whole chain.
    let reactor = Reactor::get_reactor();
    let first = create_sp_int_event(10);
    let second = create_sp_box_event::<i32>();
    let third = create_sp_quorum_event(1, 1);
    let fourth = create_sp_int_event(40);
    let order = Rc::new(RefCell::new(Vec::<&'static str>::new()));

    {
        let (a, b, log) = (first.clone(), second.clone(), order.clone());
        Fiber::create_run(move || {
            a.wait();
            log.borrow_mut().push("first");
            b.set(&(a.get() * 2));
        });
    }
    {
        let (b, c, log) = (second.clone(), third.clone(), order.clone());
        Fiber::create_run(move || {
            b.wait();
            log.borrow_mut().push("second");
            c.vote_yes();
        });
    }
    {
        let (c, d, log) = (third.clone(), fourth.clone(), order.clone());
        Fiber::create_run(move || {
            c.wait();
            log.borrow_mut().push("third");
            d.set(40);
        });
    }
    {
        let (d, log) = (fourth.clone(), order.clone());
        Fiber::create_run(move || {
            d.wait();
            log.borrow_mut().push("fourth");
        });
    }
    assert!(order.borrow().is_empty());

    first.set(10);
    assert!(order.borrow().is_empty(), "set() resumed the chain inline");
    reactor.run_loop(false, true);
    assert_eq!(*order.borrow(), ["first", "second", "third", "fourth"]);
    assert_eq!(second.get(), 20);
    for ev in [
        first.clone() as Arc<dyn EventPollable>,
        second.clone() as Arc<dyn EventPollable>,
        third.clone() as Arc<dyn EventPollable>,
        fourth.clone() as Arc<dyn EventPollable>,
    ] {
        assert_eq!(ev.status(), EventStatus::DONE);
    }
}

#[test]
fn only_the_owning_reactor_drains_the_ready_queue() {
    // The disk reactor lives on the same thread as the TLS reactor, but the
    // waiter belongs to the TLS reactor. The disk reactor's run_loop must not
    // take the queued event, or the waiter is lost.
    let reactor = Reactor::get_reactor();
    let disk = Reactor::get_disk_reactor();
    let resumed = counter();
    let ev = create_sp_int_event(1);
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait();
            r.set(r.get() + 1);
        });
    }
    ev.set(1);
    disk.run_loop(false, true);
    assert_eq!(resumed.get(), 0);
    assert_eq!(ev.status(), EventStatus::READY);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(ev.status(), EventStatus::DONE);
}

#[test]
fn quorum_finalize_event_wakes_its_finalizer_through_the_queue() {
    // finalize() parks a fiber on finalize_event_ with a timeout. When every
    // vote arrives, vote_*() sets finalize_event_, and the finalizer resumes
    // DONE without calling the finalize callback.
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);
    let quorum = create_sp_quorum_event(2, 2);
    let called = counter();
    let flag = called.clone();
    quorum.finalize(
        LONG_WAIT_US,
        Some(Box::new(move |_dangling| -> bool {
            flag.set(flag.get() + 1);
            true
        })),
    );
    let fe = quorum.finalize_event_.clone();
    assert_eq!(fe.status(), EventStatus::WAIT);
    // Timed, so it has a deadline and is on no scanned queue.
    assert_eq!(queue_lens(&reactor), (baseline.0, baseline.1, baseline.2 + 1));

    quorum.vote_yes();
    quorum.vote_no();
    assert_eq!(fe.status(), EventStatus::READY);
    reactor.run_loop(false, true);
    assert_eq!(fe.status(), EventStatus::DONE);
    assert_eq!(called.get(), 0, "the finalizer ran its timeout callback");
    assert_eq!(queue_lens(&reactor), baseline);
}

#[test]
fn a_wake_on_change_wait_can_still_time_out() {
    let reactor = Reactor::get_reactor();
    let baseline = queue_lens(&reactor);
    let resumed = counter();
    let ev = create_sp_int_event(1);
    {
        let (e, r) = (ev.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(5_000);
            r.set(r.get() + 1);
        });
    }
    let deadline = Instant::now() + Duration::from_secs(5);
    while resumed.get() == 0 {
        assert!(Instant::now() < deadline, "the timed wait never timed out");
        std::thread::sleep(Duration::from_millis(1));
        reactor.run_loop(false, true);
    }
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    assert_eq!(queue_lens(&reactor), baseline);
    // A late set neither revives it nor queues it again.
    ev.set(1);
    assert_eq!(ev.status(), EventStatus::TIMEOUT);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
}
