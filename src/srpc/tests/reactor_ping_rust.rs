// Predicate events wake on a ping (S4 step 5 of docs/dev/lion-runtime-plan.md).
// An IntEvent predicate may read state another thread publishes. The
// publisher pings an EventPing ticket after publishing; the ticket crosses to
// the owner through an ingress with an "already queued" flag, and the owner's
// next run_loop pass re-tests only the event the ticket is armed with. That
// test takes the ordinary WAIT->READY edge onto the owner's ready queue, and
// the waiter resumes in the same drain. A ping never tests the event and never
// resumes the waiter itself.
//
// Events are !Send, so the publisher threads here touch only the published
// state and the ticket, as FiberChannel's transport callbacks do.
//
// Every #[test] runs on its own thread, so each one gets a fresh thread-local
// Reactor. Baselines are still measured rather than assumed.

use srpc::reactor::{
    create_sp_int_event, event_ping, event_ping_arm, event_ping_disarm, event_ping_new,
    event_wake_report, EventPollable, EventStatus, Fiber, IntEvent, Reactor,
};
use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

fn counter() -> Rc<Cell<usize>> {
    Rc::new(Cell::new(0usize))
}

// An IntEvent whose predicate reads a flag another thread publishes, and
// counts its own evaluations.
fn flag_event(flag: &Arc<AtomicBool>, probes: &Rc<Cell<usize>>) -> Arc<IntEvent> {
    let event = create_sp_int_event(1);
    let (f, p) = (flag.clone(), probes.clone());
    *event.state_.test_.borrow_mut() = Some(Box::new(move |_value: i32| -> bool {
        p.set(p.get() + 1);
        f.load(Ordering::Acquire)
    }));
    event
}

fn park(event: &Arc<IntEvent>) -> Rc<Cell<usize>> {
    let resumed = counter();
    let (e, r) = (event.clone(), resumed.clone());
    Fiber::create_run(move || {
        e.wait();
        r.set(r.get() + 1);
    });
    resumed
}

#[test]
fn a_foreign_ping_wakes_the_waiter_on_the_owners_next_pass() {
    let reactor = Reactor::get_reactor();
    let waiting = reactor.waiting_events_.borrow().len();
    let base = event_wake_report::<()>();
    let flag = Arc::new(AtomicBool::new(false));
    let probes = counter();
    let event = flag_event(&flag, &probes);
    let ping = event_ping_new::<()>();
    event_ping_arm(&ping, &event);
    let resumed = park(&event);
    assert_eq!(event.status(), EventStatus::WAIT);
    assert_eq!(event_wake_report::<()>().armed_pings, base.armed_pings + 1);

    // The predicate event joins no scanned queue, and idle passes never
    // evaluate it.
    assert_eq!(reactor.waiting_events_.borrow().len(), waiting);
    let parked_probes = probes.get();
    for _ in 0..64 {
        reactor.run_loop(false, true);
    }
    assert_eq!(probes.get(), parked_probes, "run_loop evaluated a predicate per pass");

    // Publish and ping from another thread. The first ping makes the owner's
    // ingress non-empty; the second finds the ticket already queued.
    let (f, p) = (flag.clone(), ping.clone());
    let pings = std::thread::spawn(move || {
        f.store(true, Ordering::Release);
        (event_ping::<()>(&p), event_ping::<()>(&p))
    })
    .join()
    .unwrap();
    assert_eq!(pings, (true, false));
    // Nothing happened on the owner yet: a ping neither tests nor resumes.
    assert_eq!(event.status(), EventStatus::WAIT);
    assert_eq!(resumed.get(), 0);
    assert_eq!(probes.get(), parked_probes);

    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    assert_eq!(event.status(), EventStatus::DONE);
    // One evaluation: the drain's test of the pinged event.
    assert_eq!(probes.get(), parked_probes + 1);
    event_ping_disarm::<()>(&ping);
    assert_eq!(event_wake_report::<()>().armed_pings, base.armed_pings);
}

#[test]
fn a_publish_after_the_drains_test_queues_the_ticket_again() {
    // The drain clears the ticket's flag before it tests. A ping that lands
    // while the predicate is still false is served, the event stays waiting,
    // and the next publish's ping queues the ticket again.
    let reactor = Reactor::get_reactor();
    let flag = Arc::new(AtomicBool::new(false));
    let probes = counter();
    let event = flag_event(&flag, &probes);
    let ping = event_ping_new::<()>();
    event_ping_arm(&ping, &event);
    let resumed = park(&event);

    assert!(event_ping::<()>(&ping));
    reactor.run_loop(false, true);
    assert_eq!(event.status(), EventStatus::WAIT);
    assert_eq!(resumed.get(), 0);

    flag.store(true, Ordering::Release);
    assert!(event_ping::<()>(&ping), "the drained ticket was left marked queued");
    reactor.run_loop(false, true);
    assert_eq!(resumed.get(), 1);
    event_ping_disarm::<()>(&ping);
}

#[test]
fn tickets_share_one_ingress_per_owner() {
    // Only the ping that makes the owner's ingress non-empty reports it; S3
    // wakes the owner's driver on that edge.
    let reactor = Reactor::get_reactor();
    let (flag_a, flag_b) = (Arc::new(AtomicBool::new(false)), Arc::new(AtomicBool::new(false)));
    let probes = counter();
    let (a, b) = (flag_event(&flag_a, &probes), flag_event(&flag_b, &probes));
    let (ping_a, ping_b) = (event_ping_new::<()>(), event_ping_new::<()>());
    event_ping_arm(&ping_a, &a);
    event_ping_arm(&ping_b, &b);
    let (resumed_a, resumed_b) = (park(&a), park(&b));
    flag_a.store(true, Ordering::Release);
    flag_b.store(true, Ordering::Release);
    assert!(event_ping::<()>(&ping_a));
    assert!(!event_ping::<()>(&ping_b));
    assert!(!event_ping::<()>(&ping_a));
    reactor.run_loop(false, true);
    assert_eq!((resumed_a.get(), resumed_b.get()), (1, 1));
    assert!(event_ping::<()>(&ping_a), "the drain left the ingress marked non-empty");
    reactor.run_loop(false, true);
    event_ping_disarm::<()>(&ping_a);
    event_ping_disarm::<()>(&ping_b);
}

#[test]
fn an_unarmed_or_disarmed_ticket_tests_nothing() {
    let reactor = Reactor::get_reactor();
    let flag = Arc::new(AtomicBool::new(true));
    let probes = counter();
    let event = flag_event(&flag, &probes);
    let fresh = event_ping_new::<()>();
    assert!(!event_ping::<()>(&fresh), "a never-armed ticket reached an owner");

    // Armed, pinged, then disarmed before the drain: the drain drops it.
    let ping = event_ping_new::<()>();
    event_ping_arm(&ping, &event);
    assert!(event_ping::<()>(&ping));
    event_ping_disarm::<()>(&ping);
    let before = probes.get();
    reactor.run_loop(false, true);
    assert_eq!(probes.get(), before, "a disarmed ticket's event was tested");
    assert_eq!(event.status(), EventStatus::INIT);
}

#[test]
fn pings_after_the_owner_is_gone_are_no_ops() {
    // The owner thread arms a ticket and exits; its reactor, and with it the
    // ingress, closes. A publisher that still holds the ticket pings nothing.
    let ping = std::thread::spawn(|| {
        let _reactor = Reactor::get_reactor();
        let flag = Arc::new(AtomicBool::new(false));
        let probes = counter();
        let event = flag_event(&flag, &probes);
        let ping = event_ping_new::<()>();
        event_ping_arm(&ping, &event);
        ping
    })
    .join()
    .unwrap();
    assert!(!event_ping::<()>(&ping));
    assert!(!event_ping::<()>(&ping));
}
