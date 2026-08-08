use std::sync::atomic::Ordering;

use srpc::base::stat::AvgStat;
use srpc::rpc::connection_metrics::ConnectionMetrics;
use srpc::wire::internal_protocol::{
    encode_response_size, kInternalHeartbeatRpcId, kResponseHeaderExtFlag, kResponseSizeMask,
    response_has_extended_header, response_payload_size,
};

#[test]
fn internal_protocol_is_wire_exact_and_shared_with_frame() {
    assert_eq!(kInternalHeartbeatRpcId, i32::MIN);
    assert_eq!(kResponseHeaderExtFlag, 0x8000_0000);
    assert_eq!(kResponseSizeMask, 0x7fff_ffff);

    let plain = encode_response_size(5, false);
    let extended = encode_response_size(5, true);
    assert_eq!(plain.to_le_bytes(), [0x05, 0x00, 0x00, 0x00]);
    assert_eq!(extended.to_le_bytes(), [0x05, 0x00, 0x00, 0x80]);
    assert!(!response_has_extended_header(plain));
    assert!(response_has_extended_header(extended));
    assert_eq!(response_payload_size(plain), 5);
    assert_eq!(response_payload_size(extended), 5);

    assert_eq!(
        srpc::wire::frame::encode_response_size(23, true),
        encode_response_size(23, true)
    );
    assert_eq!(response_payload_size(-1), i32::MAX);
    assert!(response_has_extended_header(i32::MIN));
}

#[test]
fn avg_stat_preserves_zero_biased_extrema_and_snapshot_semantics() {
    let mut stat = AvgStat::new();
    assert_eq!(
        (stat.n_stat_, stat.sum_, stat.avg_, stat.max_, stat.min_),
        (0, 0, 0, 0, 0)
    );

    stat.sample(10);
    stat.sample(21);
    assert_eq!((stat.n_stat_, stat.sum_, stat.avg_), (2, 31, 15));
    assert_eq!((stat.min_, stat.max_), (0, 21));

    stat.sample(-40);
    assert_eq!((stat.n_stat_, stat.sum_, stat.avg_), (3, -9, -3));
    assert_eq!((stat.min_, stat.max_), (-40, 21));

    let snapshot = stat.peek();
    assert_eq!(snapshot.avg(), -3);
    assert_eq!(snapshot.n_stat_, 3);

    let reset_snapshot = stat.reset();
    assert_eq!(reset_snapshot.sum_, -9);
    assert_eq!(
        (stat.n_stat_, stat.sum_, stat.avg_, stat.max_, stat.min_),
        (0, 0, 0, 0, 0)
    );
}

#[test]
fn connection_metrics_tracks_the_legacy_surface() {
    let metrics = ConnectionMetrics::new();
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
    assert_eq!(metrics.min_latency_us(), 0);
    assert_eq!(metrics.max_latency_us(), 0);
    assert_eq!(metrics.success_rate_percent(), 100);
    assert_eq!(metrics.avg_latency_us(), 0);
    assert_eq!(metrics.uptime_ms(999), 0);
    assert_eq!(
        metrics.min_latency_us_field.load(Ordering::Relaxed),
        u64::MAX
    );

    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_completed_with_latency(100);
    metrics.record_request_completed_with_latency(300);
    metrics.record_request_failed();
    assert_eq!(metrics.requests_sent(), 3);
    assert_eq!(metrics.requests_completed(), 2);
    assert_eq!(metrics.requests_failed(), 1);
    assert_eq!(metrics.in_flight_requests(), 0);
    assert_eq!(metrics.success_rate_percent(), 66);
    assert_eq!(metrics.avg_latency_us(), 200);
    assert_eq!(metrics.min_latency_us(), 100);
    assert_eq!(metrics.max_latency_us(), 300);

    metrics.record_request_timeout();
    metrics.record_request_dropped();
    assert_eq!(metrics.requests_timed_out(), 1);
    assert_eq!(metrics.in_flight_requests(), 0);

    metrics.record_bytes_sent(100);
    metrics.record_bytes_sent(23);
    metrics.record_bytes_received(80);
    metrics.record_reconnect();
    metrics.record_retry_attempt();
    metrics.record_queue_drop();
    metrics.record_circuit_open_rejection();
    metrics.record_circuit_open_transition();
    metrics.record_circuit_half_open_transition();
    metrics.record_circuit_closed_transition();
    metrics.record_connect(1_000);
    assert_eq!(metrics.bytes_sent(), 123);
    assert_eq!(metrics.bytes_received(), 80);
    assert_eq!(metrics.reconnect_count(), 1);
    assert_eq!(metrics.retry_attempts(), 1);
    assert_eq!(metrics.queue_dropped_requests(), 1);
    assert_eq!(metrics.circuit_open_rejections(), 1);
    assert_eq!(metrics.circuit_open_transitions(), 1);
    assert_eq!(metrics.circuit_half_open_transitions(), 1);
    assert_eq!(metrics.circuit_closed_transitions(), 1);
    assert_eq!(metrics.connect_time_ms(), 1_000);
    assert_eq!(metrics.uptime_ms(999), 0);
    assert_eq!(metrics.uptime_ms(1_250), 250);

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
    assert_eq!(metrics.min_latency_us(), 0);
    assert_eq!(metrics.max_latency_us(), 0);
}
