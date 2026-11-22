// Composites wake on change (S4 step 4 of docs/dev/lion-runtime-plan.md).
// WaitAny and WaitAll no longer sit on run_loop's per-pass scan. Each child
// keeps weak links to its parents -- a list, because a child can be shared --
// and a test() that finds the child ready tests each parent that is waiting
// or not yet waited. The parent's test() takes the ordinary WAIT->READY edge
// onto the owner's ready queue, and its waiter resumes in the owner's next
// drain, never inside the child's set().
//
// A composite's children are never in WAIT themselves, so the link fires on
// the child's INIT->DONE edge from set(), on a direct test(), and on a child
// timer's deadline (the deadline map tests a TimeoutEvent from its creation).
//
// Every #[test] runs on its own thread, so each one gets a fresh thread-local
// Reactor. Baselines are still measured rather than assumed.

use srpc::basetypes::Time;
use srpc::reactor::{
    create_sp_box_event, create_sp_int_event, create_sp_never_event, create_sp_quorum_event,
    create_sp_timeout_event, create_sp_waitall, create_sp_waitall_from, create_sp_waitany,
    event_wake_report, reactor_prune_hwm_th_, EventPollable, EventStatus, Fiber, Reactor,
    WaitAll, WaitAny,
};
use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

// (waiting_events_, composite_events_)
fn scanned(reactor: &Reactor) -> (usize, usize) {
    (
        reactor.waiting_events_.borrow().len(),
        reactor.composite_events_.borrow().len(),
    )
}

fn counter() -> Rc<Cell<usize>> {
    Rc::new(Cell::new(0usize))
}

fn park_all(ev: &Arc<WaitAll>) -> Rc<Cell<usize>> {
    let resumed = counter();
    let (e, r) = (ev.clone(), resumed.clone());
    Fiber::create_run(move || {
        e.wait();
        r.set(r.get() + 1);
    });
    resumed
}

fn park_any(ev: &Arc<WaitAny>) -> Rc<Cell<usize>> {
    let resumed = counter();
    let (e, r) = (ev.clone(), resumed.clone());
    Fiber::create_run(move || {
        e.wait();
        r.set(r.get() + 1);
    });
    resumed
}

fn drive_until(reactor: &Reactor, what: &str, done: impl Fn() -> bool) {
    let limit = Instant::now() + Duration::from_secs(5);
    while !done() {
        assert!(Instant::now() < limit, "timed out waiting for: {what}");
        std::thread::sleep(Duration::from_millis(1));
        reactor.run_loop(false, true);
    }
}

#[test]
fn run_loop_does_not_retest_a_waiting_composite() {
    // The child's predicate counts its evaluations. The old scan tested each
    // waiting composite on every pass, and every such test evaluated the
    // child. Now nothing evaluates it until the child itself is tested.
    let reactor = Reactor::get_reactor();
    let base = scanned(&reactor);
    let probes = counter();
    let answer = Rc::new(Cell::new(false));
    let child = create_sp_int_event(1);
    {
        let (p, a) = (probes.clone(), answer.clone());
        *child.state_.test_.borrow_mut() = Some(Box::new(move |_value: i32| -> bool {
            p.set(p.get() + 1);
            a.get()
        }));
    }
    let children: Vec<Arc<dyn EventPollable>> = vec![child.clone()];
    let all = create_sp_waitall_from(&children);
    let any = create_sp_waitany(child.clone(), create_sp_never_event());
    let all_resumed = park_all(&all);
    let any_resumed = park_any(&any);
    assert_eq!(scanned(&reactor), base, "a composite joined a scanned queue");

    let after_wait = probes.get();
    for _ in 0..64 {
        reactor.run_loop(false, true);
    }
    assert_eq!(probes.get(), after_wait, "run_loop evaluated a waiting composite");
    assert_eq!(all_resumed.get() + any_resumed.get(), 0);

    // The predicate turns true, but only a test of the child tells the
    // parents. That one test queues both, and one drain resumes both.
    answer.set(true);
    reactor.run_loop(false, true);
    assert_eq!(all_resumed.get() + any_resumed.get(), 0);
    assert!(child.test());
    assert_eq!(all.status(), EventStatus::READY);
    assert_eq!(any.status(), EventStatus::READY);
    assert_eq!(all_resumed.get() + any_resumed.get(), 0, "a child test resumed a parent inline");
    reactor.run_loop(false, true);
    assert_eq!((all_resumed.get(), any_resumed.get()), (1, 1));
    assert_eq!(all.status(), EventStatus::DONE);
    assert_eq!(any.status(), EventStatus::DONE);
}

#[test]
fn child_sets_wake_a_waitall_only_when_complete_and_only_in_the_drain() {
    // test_and_event.cc's BasicAndEvent and ThreeEventAnd, across three
    // child kinds. Children are never in WAIT: each set() moves one
    // INIT->DONE, and that edge is what reaches the parent.
    let reactor = Reactor::get_reactor();
    let a = create_sp_int_event(1);
    let b = create_sp_box_event::<i32>();
    let q = create_sp_quorum_event(1, 1);
    let children: Vec<Arc<dyn EventPollable>> = vec![a.clone(), b.clone(), q.clone()];
    let all = create_sp_waitall_from(&children);
    let resumed = park_all(&all);

    b.set(&5);
    assert_eq!(b.status(), EventStatus::DONE);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 0, "complete after a partial set");
    assert_eq!(all.status(), EventStatus::WAIT);
    q.vote_yes();
    assert_eq!(q.status(), EventStatus::DONE);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 0, "complete after a partial set");

    a.set(1);
    assert_eq!(a.status(), EventStatus::DONE);
    assert_eq!(all.status(), EventStatus::READY);
    assert_eq!(resumed.get(), 0, "set() resumed the composite's waiter inline");
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(all.status(), EventStatus::DONE);
}

#[test]
fn waitany_wakes_on_its_second_child() {
    let reactor = Reactor::get_reactor();
    let a = create_sp_int_event(1);
    let b = create_sp_int_event(1);
    let any = create_sp_waitany(a.clone(), b.clone());
    let resumed = park_any(&any);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 0);
    b.set(1);
    assert_eq!(resumed.get(), 0);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(any.status(), EventStatus::DONE);
    assert_eq!(a.status(), EventStatus::INIT);
}

#[test]
fn a_child_timer_wakes_its_parent_at_the_timer_deadline() {
    // test_and_event.cc's MixedEventTypes: the TimeoutEvent child is never
    // waited on itself. The deadline map tests it at its deadline, and that
    // INIT->DONE edge tells the WaitAll. A WaitAny over a NeverEvent and a
    // timer resumes at the timer, too.
    let reactor = Reactor::get_reactor();
    let int_child = create_sp_int_event(1);
    let timer = create_sp_timeout_event(200_000);
    let children: Vec<Arc<dyn EventPollable>> = vec![int_child.clone(), timer.clone()];
    let all = create_sp_waitall_from(&children);
    let short_timer = create_sp_timeout_event(100_000);
    let any = create_sp_waitany(create_sp_never_event(), short_timer.clone());
    let all_resumed = park_all(&all);
    let any_resumed = park_any(&any);
    int_child.set(1);
    reactor.run_loop(false, true);
    assert_eq!(all_resumed.get(), 0);

    drive_until(&reactor, "both timers", || all_resumed.get() == 1 && any_resumed.get() == 1);
    assert!(Time::now(true) > timer.wakeup_time_);
    assert_eq!(timer.status(), EventStatus::DONE);
    assert_eq!(short_timer.status(), EventStatus::DONE);
    assert_eq!(all.status(), EventStatus::DONE);
    assert_eq!(any.status(), EventStatus::DONE);
}

#[test]
fn a_shared_child_wakes_every_parent() {
    // One child in two parents: a WaitAll (with a second child already set)
    // and a WaitAny. The child keeps a list of parents, and its one set()
    // queues both waiters for the same drain.
    let reactor = Reactor::get_reactor();
    let shared = create_sp_int_event(1);
    let other = create_sp_int_event(1);
    let children: Vec<Arc<dyn EventPollable>> = vec![shared.clone(), other.clone()];
    let all = create_sp_waitall_from(&children);
    let any = create_sp_waitany(create_sp_never_event(), shared.clone());
    let all_resumed = park_all(&all);
    let any_resumed = park_any(&any);
    other.set(1);
    reactor.run_loop(false, true);
    assert_eq!((all_resumed.get(), any_resumed.get()), (0, 0));

    shared.set(1);
    assert_eq!(all.status(), EventStatus::READY);
    assert_eq!(any.status(), EventStatus::READY);
    reactor.run_loop(false, true);
    assert_eq!((all_resumed.get(), any_resumed.get()), (1, 1));
}

#[test]
fn nested_composites_propagate_through_an_unwaited_middle() {
    // Only the outer WaitAll is waited. The inner WaitAny is its child and
    // is never in WAIT; its own INIT->DONE edge is what reaches the outer.
    let reactor = Reactor::get_reactor();
    let a = create_sp_int_event(1);
    let b = create_sp_int_event(1);
    let c = create_sp_int_event(1);
    let inner = create_sp_waitany(a.clone(), b.clone());
    let children: Vec<Arc<dyn EventPollable>> = vec![inner.clone(), c.clone()];
    let outer = create_sp_waitall_from(&children);
    let resumed = park_all(&outer);
    c.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 0);
    assert_eq!(inner.status(), EventStatus::INIT);

    a.set(1);
    assert_eq!(inner.status(), EventStatus::DONE);
    assert_eq!(outer.status(), EventStatus::READY);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
}

#[test]
fn add_event_links_the_new_child() {
    let reactor = Reactor::get_reactor();
    let all = create_sp_waitall();
    let a = create_sp_int_event(1);
    let b = create_sp_int_event(1);
    all.add_event(a.clone());
    all.add_event(b.clone());
    let resumed = park_all(&all);
    a.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 0);
    b.set(1);
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
}

#[test]
fn a_timed_composite_still_times_out() {
    let reactor = Reactor::get_reactor();
    let a = create_sp_int_event(1);
    let b = create_sp_int_event(1);
    let children: Vec<Arc<dyn EventPollable>> = vec![a.clone(), b.clone()];
    let all = create_sp_waitall_from(&children);
    let resumed = counter();
    {
        let (e, r) = (all.clone(), resumed.clone());
        Fiber::create_run(move || {
            e.wait_timeout(10_000);
            r.set(r.get() + 1);
        });
    }
    a.set(1);
    drive_until(&reactor, "the composite timeout", || resumed.get() == 1);
    assert_eq!(all.status(), EventStatus::TIMEOUT);
    // TIMEOUT is sticky: completing it later changes nothing.
    b.set(1);
    reactor.run_loop(false, true);
    assert_eq!(all.status(), EventStatus::TIMEOUT);
    assert_eq!(resumed.get(), 1);
}

// all_events_ keeps every event it registered alive until it prunes, and it
// prunes only at a threshold, so how many short-lived composites are still
// alive at any moment depends on that schedule. Prune it on every round, twice
// (a child is retained on the pass that frees its parent), so each composite
// here really dies before the next is made, and the bounds below are exact.
fn prune_now(reactor: &Reactor) {
    for _ in 0..2 {
        reactor_prune_hwm_th_.with(|hwm| hwm.set(0usize));
        reactor.prune_finished_events();
    }
}

#[test]
fn parent_links_do_not_accumulate() {
    // Links are weak and pruned lazily. Short-lived composites leave child
    // entries behind, and a long-lived child shared by all of them collects
    // dead links. The children stay alive, so every round adds two entries
    // at fresh addresses. With at most one composite alive at a time, at
    // most 3 entries are live. The map sweep then keeps at most 2 * 3 + 64
    // entries (it re-arms at twice the live count plus 64), each holding one
    // link, and the long-lived child's list keeps at most 10 links (twice
    // the live parents plus 8). Without the sweeps there would be 2 * ROUNDS
    // entries and 3 * ROUNDS links.
    const ROUNDS: usize = 1000;
    let reactor = Reactor::get_reactor();
    prune_now(&reactor);
    let base = event_wake_report::<()>();
    let long_lived = create_sp_int_event(1);
    let mut kept: Vec<Arc<dyn EventPollable>> = Vec::new();
    for _ in 0..ROUNDS {
        let x = create_sp_int_event(1);
        let y = create_sp_int_event(1);
        let children: Vec<Arc<dyn EventPollable>> = vec![x.clone(), y.clone(), long_lived.clone()];
        drop(create_sp_waitall_from(&children));
        kept.push(x);
        kept.push(y);
        prune_now(&reactor);
    }
    let after = event_wake_report::<()>();
    assert!(
        after.composite_children <= base.composite_children + 2 * 3 + 64 + 1,
        "{} child entries kept after {} short-lived composites",
        after.composite_children,
        ROUNDS
    );
    assert!(
        after.parent_links <= base.parent_links + 2 * 3 + 64 + 1 + 10,
        "{} parent links kept after {} short-lived composites",
        after.parent_links,
        ROUNDS
    );
    assert_eq!(kept.len(), 2 * ROUNDS);
}

#[test]
fn a_long_lived_child_prunes_its_dead_parents() {
    // One child shared by many short-lived parents and nothing else, so the
    // map never grows and only the child's own list can shed dead links.
    // With at most one parent alive, the list re-arms at 10 links.
    const ROUNDS: usize = 1000;
    let reactor = Reactor::get_reactor();
    prune_now(&reactor);
    let base = event_wake_report::<()>();
    let child = create_sp_int_event(1);
    for _ in 0..ROUNDS {
        let children: Vec<Arc<dyn EventPollable>> = vec![child.clone()];
        drop(create_sp_waitall_from(&children));
        prune_now(&reactor);
    }
    let after = event_wake_report::<()>();
    assert_eq!(after.composite_children, base.composite_children + 1);
    assert!(
        after.parent_links <= base.parent_links + 10,
        "{} parent links kept for one child after {} short-lived parents",
        after.parent_links,
        ROUNDS
    );
}
