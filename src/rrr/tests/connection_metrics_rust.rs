use std::mem::{align_of, offset_of, size_of};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;

use rrr::connection_metrics::ConnectionMetrics;

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn public_repr_c_layout_is_eighteen_atomic_u64_fields_in_order() {
    assert_send_sync::<ConnectionMetrics>();

    const ATOMIC_SIZE: usize = size_of::<AtomicU64>();

    assert_eq!(ATOMIC_SIZE, size_of::<u64>());
    assert_eq!(align_of::<AtomicU64>(), align_of::<u64>());
    assert_eq!(size_of::<ConnectionMetrics>(), 18 * size_of::<u64>());
    assert_eq!(align_of::<ConnectionMetrics>(), align_of::<u64>());

    assert_eq!(offset_of!(ConnectionMetrics, requests_sent_field), 0);
    assert_eq!(
        offset_of!(ConnectionMetrics, requests_completed_field),
        ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, requests_failed_field),
        2 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, requests_timed_out_field),
        3 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, in_flight_requests_field),
        4 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, bytes_sent_field),
        5 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, bytes_received_field),
        6 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, reconnect_count_field),
        7 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, retry_attempts_field),
        8 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, queue_dropped_requests_field),
        9 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, circuit_open_rejections_field),
        10 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, circuit_open_transitions_field),
        11 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, circuit_half_open_transitions_field),
        12 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, circuit_closed_transitions_field),
        13 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, connect_time_ms_field),
        14 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, total_latency_us_field),
        15 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, min_latency_us_field),
        16 * ATOMIC_SIZE
    );
    assert_eq!(
        offset_of!(ConnectionMetrics, max_latency_us_field),
        17 * ATOMIC_SIZE
    );
}

#[test]
fn request_and_latency_metrics_preserve_the_production_state_machine() {
    let metrics = ConnectionMetrics::new();
    assert_eq!(metrics.requests_sent(), 0);
    assert_eq!(metrics.requests_completed(), 0);
    assert_eq!(metrics.requests_failed(), 0);
    assert_eq!(metrics.requests_timed_out(), 0);
    assert_eq!(metrics.in_flight_requests(), 0);
    assert_eq!(metrics.min_latency_us(), 0);
    assert_eq!(metrics.max_latency_us(), 0);
    assert_eq!(metrics.avg_latency_us(), 0);
    assert_eq!(metrics.success_rate_percent(), 100);
    assert_eq!(
        metrics.min_latency_us_field.load(Ordering::Relaxed),
        u64::MAX
    );

    metrics.record_request_dropped();
    assert_eq!(metrics.in_flight_requests(), 0);

    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_sent();
    assert_eq!(metrics.requests_sent(), 3);
    assert_eq!(metrics.in_flight_requests(), 3);

    metrics.record_request_completed_with_latency(30);
    metrics.record_request_completed_with_latency(10);
    assert_eq!(metrics.requests_completed(), 2);
    assert_eq!(metrics.in_flight_requests(), 1);
    assert_eq!(metrics.total_latency_us_field.load(Ordering::Relaxed), 40);
    assert_eq!(metrics.min_latency_us(), 10);
    assert_eq!(metrics.max_latency_us(), 30);
    assert_eq!(metrics.avg_latency_us(), 20);
    assert_eq!(metrics.success_rate_percent(), 66);

    metrics.record_request_failed();
    assert_eq!(metrics.requests_failed(), 1);
    assert_eq!(metrics.in_flight_requests(), 0);

    metrics.record_request_timeout();
    metrics.record_request_dropped();
    assert_eq!(metrics.requests_timed_out(), 1);
    assert_eq!(metrics.in_flight_requests(), 0);

    metrics.record_request_completed();
    assert_eq!(metrics.requests_completed(), 3);
    assert_eq!(metrics.in_flight_requests(), 0);
    assert_eq!(metrics.avg_latency_us(), 13);
    assert_eq!(metrics.success_rate_percent(), 100);
}

#[test]
fn transport_reliability_uptime_and_reset_metrics_preserve_behavior() {
    let metrics = ConnectionMetrics::new();

    metrics.record_bytes_sent(11);
    metrics.record_bytes_sent(7);
    metrics.record_bytes_received(23);
    metrics.record_reconnect();
    metrics.record_retry_attempt();
    metrics.record_queue_drop();
    metrics.record_circuit_open_rejection();
    metrics.record_circuit_open_transition();
    metrics.record_circuit_half_open_transition();
    metrics.record_circuit_closed_transition();

    assert_eq!(metrics.bytes_sent(), 18);
    assert_eq!(metrics.bytes_received(), 23);
    assert_eq!(metrics.reconnect_count(), 1);
    assert_eq!(metrics.retry_attempts(), 1);
    assert_eq!(metrics.queue_dropped_requests(), 1);
    assert_eq!(metrics.circuit_open_rejections(), 1);
    assert_eq!(metrics.circuit_open_transitions(), 1);
    assert_eq!(metrics.circuit_half_open_transitions(), 1);
    assert_eq!(metrics.circuit_closed_transitions(), 1);

    assert_eq!(metrics.connect_time_ms(), 0);
    assert_eq!(metrics.uptime_ms(1234), 0);
    metrics.record_connect(1000);
    assert_eq!(metrics.connect_time_ms(), 1000);
    assert_eq!(metrics.uptime_ms(999), 0);
    assert_eq!(metrics.uptime_ms(1000), 0);
    assert_eq!(metrics.uptime_ms(1123), 123);

    metrics.record_request_sent();
    metrics.record_request_completed_with_latency(17);
    metrics.reset();

    assert_eq!(metrics.requests_sent(), 0);
    assert_eq!(metrics.requests_completed(), 0);
    assert_eq!(metrics.requests_failed(), 0);
    assert_eq!(metrics.requests_timed_out(), 0);
    assert_eq!(metrics.in_flight_requests(), 0);
    assert_eq!(metrics.bytes_sent(), 0);
    assert_eq!(metrics.bytes_received(), 0);
    assert_eq!(metrics.reconnect_count(), 0);
    assert_eq!(metrics.retry_attempts(), 0);
    assert_eq!(metrics.queue_dropped_requests(), 0);
    assert_eq!(metrics.circuit_open_rejections(), 0);
    assert_eq!(metrics.circuit_open_transitions(), 0);
    assert_eq!(metrics.circuit_half_open_transitions(), 0);
    assert_eq!(metrics.circuit_closed_transitions(), 0);
    assert_eq!(metrics.connect_time_ms(), 0);
    assert_eq!(metrics.total_latency_us_field.load(Ordering::Relaxed), 0);
    assert_eq!(
        metrics.min_latency_us_field.load(Ordering::Relaxed),
        u64::MAX
    );
    assert_eq!(metrics.min_latency_us(), 0);
    assert_eq!(metrics.max_latency_us(), 0);
    assert_eq!(metrics.avg_latency_us(), 0);
    assert_eq!(metrics.success_rate_percent(), 100);
}

#[test]
fn fetch_add_has_unsigned_wrapping_parity() {
    let metrics = ConnectionMetrics::new();
    metrics.bytes_sent_field.store(u64::MAX, Ordering::Relaxed);
    metrics.record_bytes_sent(1);
    assert_eq!(metrics.bytes_sent(), 0);

    metrics.requests_sent_field.store(u64::MAX, Ordering::Relaxed);
    metrics.record_request_sent();
    assert_eq!(metrics.requests_sent(), 0);
    assert_eq!(metrics.in_flight_requests(), 1);

    metrics
        .requests_completed_field
        .store(u64::MAX, Ordering::Relaxed);
    metrics.requests_sent_field.store(3, Ordering::Relaxed);
    assert_eq!(
        metrics.success_rate_percent(),
        u64::MAX.wrapping_mul(100) / 3
    );
}

#[test]
fn concurrent_updates_are_atomic_and_in_flight_saturates() {
    const THREADS: usize = 8;
    const OPS_PER_THREAD: usize = 2_000;
    const ROUNDS: usize = 3;

    for _ in 0..ROUNDS {
        let metrics = Arc::new(ConnectionMetrics::new());
        let start = Arc::new(Barrier::new(THREADS));
        let mut workers = Vec::with_capacity(THREADS);

        for thread_index in 0..THREADS {
            let metrics = Arc::clone(&metrics);
            let start = Arc::clone(&start);
            workers.push(thread::spawn(move || {
                let latency = (thread_index + 1) as u64;
                start.wait();
                for _ in 0..OPS_PER_THREAD {
                    metrics.record_request_sent();
                    metrics.record_request_completed_with_latency(latency);
                    metrics.record_request_sent();
                    metrics.record_request_failed();
                    metrics.record_request_sent();
                    metrics.record_request_timeout();
                    metrics.record_request_sent();
                    metrics.record_request_dropped();
                    metrics.record_bytes_sent(3);
                    metrics.record_bytes_received(5);
                    metrics.record_reconnect();
                    metrics.record_retry_attempt();
                    metrics.record_queue_drop();
                    metrics.record_circuit_open_rejection();
                    metrics.record_circuit_open_transition();
                    metrics.record_circuit_half_open_transition();
                    metrics.record_circuit_closed_transition();
                }
            }));
        }

        for worker in workers {
            worker.join().expect("metrics worker panicked");
        }

        let updates = (THREADS * OPS_PER_THREAD) as u64;
        let latency_total =
            OPS_PER_THREAD as u64 * (1..=THREADS as u64).sum::<u64>();
        assert_eq!(metrics.requests_sent(), 4 * updates);
        assert_eq!(metrics.requests_completed(), updates);
        assert_eq!(metrics.requests_failed(), updates);
        assert_eq!(metrics.requests_timed_out(), updates);
        assert_eq!(metrics.in_flight_requests(), 0);
        assert_eq!(metrics.bytes_sent(), 3 * updates);
        assert_eq!(metrics.bytes_received(), 5 * updates);
        assert_eq!(metrics.reconnect_count(), updates);
        assert_eq!(metrics.retry_attempts(), updates);
        assert_eq!(metrics.queue_dropped_requests(), updates);
        assert_eq!(metrics.circuit_open_rejections(), updates);
        assert_eq!(metrics.circuit_open_transitions(), updates);
        assert_eq!(metrics.circuit_half_open_transitions(), updates);
        assert_eq!(metrics.circuit_closed_transitions(), updates);
        assert_eq!(
            metrics.total_latency_us_field.load(Ordering::Relaxed),
            latency_total
        );
        assert_eq!(metrics.min_latency_us(), 1);
        assert_eq!(metrics.max_latency_us(), THREADS as u64);
        assert_eq!(metrics.avg_latency_us(), latency_total / updates);
        assert_eq!(metrics.success_rate_percent(), 25);

        for _ in 0..THREADS {
            metrics.record_request_dropped();
        }
        assert_eq!(metrics.in_flight_requests(), 0);
    }
}
