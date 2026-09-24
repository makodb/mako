use srpc::fiber::this_fiber;
use srpc::reactor::{Fiber, Reactor};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};

#[test]
fn yield_suspends_the_current_fiber_until_the_owner_resumes_it() {
    assert!(!this_fiber::in_fiber_context());
    assert!(this_fiber::current().is_none());
    this_fiber::r#yield();
    let steps = Rc::new(RefCell::new(Vec::new()));
    let observed = steps.clone();
    let fiber = Fiber::create_run(move || {
        let current = this_fiber::current().expect("canonical running fiber");
        assert_eq!(this_fiber::get_id(), current.id.get());
        observed.borrow_mut().push(1);
        this_fiber::r#yield();
        observed.borrow_mut().push(3);
        this_fiber::r#yield();
        observed.borrow_mut().push(5);
    });
    assert_eq!(*steps.borrow(), [1]);
    steps.borrow_mut().push(2);
    Reactor::get_reactor().continue_fiber(&fiber);
    assert_eq!(*steps.borrow(), [1, 2, 3]);
    steps.borrow_mut().push(4);
    Reactor::get_reactor().continue_fiber(&fiber);
    assert_eq!(*steps.borrow(), [1, 2, 3, 4, 5]);
    assert!(!this_fiber::in_fiber_context());
}

#[test]
fn sleeping_fiber_allows_another_fiber_to_run_and_resumes_after_its_deadline() {
    let done = Rc::new(Cell::new(false));
    let finished = done.clone();
    let started = Instant::now();
    Fiber::create_run(move || {
        this_fiber::sleep_ms(25);
        assert!(started.elapsed() >= Duration::from_millis(25));
        finished.set(true);
    });
    assert!(!done.get(), "sleep must suspend before completing");
    let peer_ran = Rc::new(Cell::new(false));
    let peer = peer_ran.clone();
    Fiber::create_run(move || peer.set(true));
    assert!(peer_ran.get(), "another fiber progresses during the wait");
    let reactor = Reactor::get_reactor();
    while !done.get() && started.elapsed() < Duration::from_secs(2) {
        reactor.run_loop(false, true);
        std::thread::yield_now();
    }
    assert!(done.get(), "the owner must wake the timer fiber");
}

#[test]
fn zero_and_past_deadlines_return_without_a_fiber() {
    this_fiber::sleep_us(0);
    this_fiber::sleep_ms(0);
    this_fiber::sleep_s(0);
    this_fiber::sleep_until_us(0);
    assert!(!this_fiber::in_fiber_context());
}
