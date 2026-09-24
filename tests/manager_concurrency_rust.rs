use srpc::circuit_breaker::{CircuitBreaker, CircuitBreakerConfig};
use srpc::connection_state::{ConnectionState, ConnectionStateMachine};
use srpc::heartbeat::{HeartbeatConfig, HeartbeatManager, HeartbeatTimeoutCallback};
use srpc::request_queue::{OverflowStrategy, QueuedRequest, RequestQueue,
    RequestQueueConfig};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Barrier};

#[test]
fn only_one_concurrent_request_wins_the_half_open_probe() {
    let breaker = Arc::new(CircuitBreaker::new(CircuitBreakerConfig {
        enabled: true, failure_threshold: 1, success_threshold: 1, timeout_ms: 0,
    }));
    breaker.record_failure_at(100);
    let barrier = Arc::new(Barrier::new(17));
    let workers: Vec<_> = (0..16).map(|_| {
        let breaker = breaker.clone();
        let barrier = barrier.clone();
        std::thread::spawn(move || { barrier.wait(); breaker.allow_request_at(100) })
    }).collect();
    barrier.wait();
    let winners = workers.into_iter().map(|worker| usize::from(worker.join().unwrap())).sum::<usize>();
    assert_eq!(winners, 1);
    assert!(breaker.is_half_open());
    breaker.record_success();
    assert!(breaker.is_closed());
}

#[test]
fn a_state_transition_and_its_callback_happen_once_under_contention() {
    let callbacks = Arc::new(AtomicUsize::new(0));
    let observed = callbacks.clone();
    let mut machine = ConnectionStateMachine::new();
    machine.set_on_state_change(Some(Box::new(move |from, to| {
        assert_eq!(from, ConnectionState::NEW);
        assert_eq!(to, ConnectionState::CONNECTING);
        observed.fetch_add(1, Ordering::SeqCst);
    })));
    let machine = Arc::new(machine);
    let barrier = Arc::new(Barrier::new(9));
    let workers: Vec<_> = (0..8).map(|_| {
        let machine = machine.clone();
        let barrier = barrier.clone();
        std::thread::spawn(move || { barrier.wait(); machine.transition_to(ConnectionState::CONNECTING) })
    }).collect();
    barrier.wait();
    let winners = workers.into_iter().map(|worker| usize::from(worker.join().unwrap())).sum::<usize>();
    assert_eq!(winners, 1);
    assert_eq!(callbacks.load(Ordering::SeqCst), 1);
}

#[test]
fn heartbeat_timeout_callback_can_reset_state_and_fires_once() {
    let callbacks = Arc::new(AtomicUsize::new(0));
    let observed = callbacks.clone();
    let manager = Arc::new_cyclic(|weak: &std::sync::Weak<HeartbeatManager>| {
        let manager = HeartbeatManager::new(&HeartbeatConfig {
            enabled: true, interval_ms: 1, timeout_ms: 0, max_missed: 1,
        });
        let weak = weak.clone();
        manager.set_on_timeout(Some(Box::new(move || {
            observed.fetch_add(1, Ordering::SeqCst);
            weak.upgrade().unwrap().reset();
        })));
        manager
    });
    manager.on_heartbeat_sent_at(100);
    let barrier = Arc::new(Barrier::new(9));
    let workers: Vec<_> = (0..8).map(|_| {
        let manager = manager.clone();
        let barrier = barrier.clone();
        std::thread::spawn(move || { barrier.wait(); manager.check_timeout_at(100) })
    }).collect();
    barrier.wait();
    let timeouts = workers.into_iter().map(|worker| usize::from(worker.join().unwrap())).sum::<usize>();
    assert_eq!(timeouts, 1);
    assert_eq!(callbacks.load(Ordering::SeqCst), 1);
    assert!(!manager.is_timed_out());
    assert!(!manager.is_pending_pong());
}

#[test]
fn heartbeat_callback_can_uninstall_itself() {
    let callbacks = Arc::new(AtomicUsize::new(0));
    let observed = callbacks.clone();
    let manager = Arc::new_cyclic(|weak: &std::sync::Weak<HeartbeatManager>| {
        let manager = HeartbeatManager::new(&HeartbeatConfig {
            enabled: true, interval_ms: 1, timeout_ms: 0, max_missed: 1,
        });
        let weak = weak.clone();
        manager.set_on_timeout(Some(Box::new(move || {
            observed.fetch_add(1, Ordering::SeqCst);
            weak.upgrade().unwrap().set_on_timeout(HeartbeatTimeoutCallback::default());
        })));
        manager
    });
    manager.on_heartbeat_sent_at(100);
    assert!(manager.check_timeout_at(100));
    manager.reset();
    manager.on_heartbeat_sent_at(200);
    assert!(manager.check_timeout_at(200));
    assert_eq!(callbacks.load(Ordering::SeqCst), 1);
}

#[test]
fn concurrent_queue_producers_preserve_capacity_and_rejection_count() {
    let queue = Arc::new(RequestQueue::with_config(RequestQueueConfig {
        max_size: 64, default_ttl_ms: 1000, overflow_strategy: OverflowStrategy::DROP_NEWEST,
        enabled: true,
    }));
    let rejected = Arc::new(AtomicUsize::new(0));
    let workers: Vec<_> = (0..8).map(|_| {
        let queue = queue.clone();
        let rejected = rejected.clone();
        std::thread::spawn(move || {
            let mut accepted = 0;
            for _ in 0..32 {
                let rejected = rejected.clone();
                let mut request = QueuedRequest::new();
                request.callback = Some(Box::new(move |_| {
                    rejected.fetch_add(1, Ordering::SeqCst);
                }));
                accepted += usize::from(queue.enqueue(request));
            }
            accepted
        })
    }).collect();
    let accepted = workers.into_iter().map(|worker| worker.join().unwrap()).sum::<usize>();
    assert_eq!(accepted, 64);
    assert_eq!(queue.size(), 64);
    assert_eq!(rejected.load(Ordering::SeqCst), 192);
}
