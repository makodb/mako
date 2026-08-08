//! Relaxed-atomic RPC connection metrics.
//!
//! Field and method names intentionally match the legacy
//! `rrr.connection_metrics` module because this Rust source is also
//! transpiled to that C++ API.

pub use std::sync::atomic::{AtomicU64, Ordering};

pub struct ConnectionMetrics {
    pub requests_sent_field: AtomicU64,
    pub requests_completed_field: AtomicU64,
    pub requests_failed_field: AtomicU64,
    pub requests_timed_out_field: AtomicU64,
    pub in_flight_requests_field: AtomicU64,

    pub bytes_sent_field: AtomicU64,
    pub bytes_received_field: AtomicU64,

    pub reconnect_count_field: AtomicU64,
    pub retry_attempts_field: AtomicU64,
    pub queue_dropped_requests_field: AtomicU64,
    pub circuit_open_rejections_field: AtomicU64,
    pub circuit_open_transitions_field: AtomicU64,
    pub circuit_half_open_transitions_field: AtomicU64,
    pub circuit_closed_transitions_field: AtomicU64,
    pub connect_time_ms_field: AtomicU64,

    pub total_latency_us_field: AtomicU64,
    pub min_latency_us_field: AtomicU64,
    pub max_latency_us_field: AtomicU64,
}

impl ConnectionMetrics {
    pub fn new() -> ConnectionMetrics {
        ConnectionMetrics {
            requests_sent_field: AtomicU64::new(0),
            requests_completed_field: AtomicU64::new(0),
            requests_failed_field: AtomicU64::new(0),
            requests_timed_out_field: AtomicU64::new(0),
            in_flight_requests_field: AtomicU64::new(0),
            bytes_sent_field: AtomicU64::new(0),
            bytes_received_field: AtomicU64::new(0),
            reconnect_count_field: AtomicU64::new(0),
            retry_attempts_field: AtomicU64::new(0),
            queue_dropped_requests_field: AtomicU64::new(0),
            circuit_open_rejections_field: AtomicU64::new(0),
            circuit_open_transitions_field: AtomicU64::new(0),
            circuit_half_open_transitions_field: AtomicU64::new(0),
            circuit_closed_transitions_field: AtomicU64::new(0),
            connect_time_ms_field: AtomicU64::new(0),
            total_latency_us_field: AtomicU64::new(0),
            min_latency_us_field: AtomicU64::new(u64::MAX),
            max_latency_us_field: AtomicU64::new(0),
        }
    }

    pub fn requests_sent(&self) -> u64 {
        self.requests_sent_field.load(Ordering::Relaxed)
    }

    pub fn requests_completed(&self) -> u64 {
        self.requests_completed_field.load(Ordering::Relaxed)
    }

    pub fn requests_failed(&self) -> u64 {
        self.requests_failed_field.load(Ordering::Relaxed)
    }

    pub fn requests_timed_out(&self) -> u64 {
        self.requests_timed_out_field.load(Ordering::Relaxed)
    }

    pub fn in_flight_requests(&self) -> u64 {
        self.in_flight_requests_field.load(Ordering::Relaxed)
    }

    pub fn bytes_sent(&self) -> u64 {
        self.bytes_sent_field.load(Ordering::Relaxed)
    }

    pub fn bytes_received(&self) -> u64 {
        self.bytes_received_field.load(Ordering::Relaxed)
    }

    pub fn reconnect_count(&self) -> u64 {
        self.reconnect_count_field.load(Ordering::Relaxed)
    }

    pub fn retry_attempts(&self) -> u64 {
        self.retry_attempts_field.load(Ordering::Relaxed)
    }

    pub fn queue_dropped_requests(&self) -> u64 {
        self.queue_dropped_requests_field.load(Ordering::Relaxed)
    }

    pub fn circuit_open_rejections(&self) -> u64 {
        self.circuit_open_rejections_field.load(Ordering::Relaxed)
    }

    pub fn circuit_open_transitions(&self) -> u64 {
        self.circuit_open_transitions_field.load(Ordering::Relaxed)
    }

    pub fn circuit_half_open_transitions(&self) -> u64 {
        self.circuit_half_open_transitions_field
            .load(Ordering::Relaxed)
    }

    pub fn circuit_closed_transitions(&self) -> u64 {
        self.circuit_closed_transitions_field
            .load(Ordering::Relaxed)
    }

    pub fn connect_time_ms(&self) -> u64 {
        self.connect_time_ms_field.load(Ordering::Relaxed)
    }

    pub fn min_latency_us(&self) -> u64 {
        let min = self.min_latency_us_field.load(Ordering::Relaxed);
        if min == u64::MAX {
            0
        } else {
            min
        }
    }

    pub fn max_latency_us(&self) -> u64 {
        self.max_latency_us_field.load(Ordering::Relaxed)
    }

    pub fn success_rate_percent(&self) -> u64 {
        let completed = self.requests_completed_field.load(Ordering::Relaxed);
        let total = self.requests_sent_field.load(Ordering::Relaxed);
        if total == 0 {
            return 100;
        }
        (completed * 100) / total
    }

    pub fn avg_latency_us(&self) -> u64 {
        let completed = self.requests_completed_field.load(Ordering::Relaxed);
        if completed == 0 {
            return 0;
        }
        self.total_latency_us_field.load(Ordering::Relaxed) / completed
    }

    pub fn uptime_ms(&self, current_time_ms: u64) -> u64 {
        let connect_time = self.connect_time_ms_field.load(Ordering::Relaxed);
        if connect_time == 0 || current_time_ms < connect_time {
            return 0;
        }
        current_time_ms - connect_time
    }

    pub fn record_request_sent(&self) {
        self.requests_sent_field.store(
            self.requests_sent_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
        self.in_flight_requests_field.store(
            self.in_flight_requests_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_request_completed_with_latency(&self, latency_us: u64) {
        self.requests_completed_field.store(
            self.requests_completed_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
        self.decrement_in_flight();
        self.total_latency_us_field.store(
            self.total_latency_us_field.load(Ordering::Relaxed) + latency_us,
            Ordering::Relaxed,
        );

        let current_min = self.min_latency_us_field.load(Ordering::Relaxed);
        if latency_us < current_min {
            self.min_latency_us_field
                .store(latency_us, Ordering::Relaxed);
        }

        let current_max = self.max_latency_us_field.load(Ordering::Relaxed);
        if latency_us > current_max {
            self.max_latency_us_field
                .store(latency_us, Ordering::Relaxed);
        }
    }

    pub fn record_request_completed(&self) {
        self.requests_completed_field.store(
            self.requests_completed_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
        self.decrement_in_flight();
    }

    pub fn record_request_failed(&self) {
        self.requests_failed_field.store(
            self.requests_failed_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
        self.decrement_in_flight();
    }

    pub fn record_request_timeout(&self) {
        self.requests_timed_out_field.store(
            self.requests_timed_out_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
        self.decrement_in_flight();
    }

    pub fn record_request_dropped(&self) {
        self.decrement_in_flight();
    }

    pub fn record_bytes_sent(&self, bytes: u64) {
        self.bytes_sent_field.store(
            self.bytes_sent_field.load(Ordering::Relaxed) + bytes,
            Ordering::Relaxed,
        );
    }

    pub fn record_bytes_received(&self, bytes: u64) {
        self.bytes_received_field.store(
            self.bytes_received_field.load(Ordering::Relaxed) + bytes,
            Ordering::Relaxed,
        );
    }

    pub fn record_reconnect(&self) {
        self.reconnect_count_field.store(
            self.reconnect_count_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_retry_attempt(&self) {
        self.retry_attempts_field.store(
            self.retry_attempts_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_queue_drop(&self) {
        self.queue_dropped_requests_field.store(
            self.queue_dropped_requests_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_circuit_open_rejection(&self) {
        self.circuit_open_rejections_field.store(
            self.circuit_open_rejections_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_circuit_open_transition(&self) {
        self.circuit_open_transitions_field.store(
            self.circuit_open_transitions_field.load(Ordering::Relaxed) + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_circuit_half_open_transition(&self) {
        self.circuit_half_open_transitions_field.store(
            self.circuit_half_open_transitions_field
                .load(Ordering::Relaxed)
                + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_circuit_closed_transition(&self) {
        self.circuit_closed_transitions_field.store(
            self.circuit_closed_transitions_field
                .load(Ordering::Relaxed)
                + 1,
            Ordering::Relaxed,
        );
    }

    pub fn record_connect(&self, current_time_ms: u64) {
        self.connect_time_ms_field
            .store(current_time_ms, Ordering::Relaxed);
    }

    pub fn reset(&self) {
        self.requests_sent_field.store(0, Ordering::Relaxed);
        self.requests_completed_field.store(0, Ordering::Relaxed);
        self.requests_failed_field.store(0, Ordering::Relaxed);
        self.requests_timed_out_field.store(0, Ordering::Relaxed);
        self.in_flight_requests_field.store(0, Ordering::Relaxed);
        self.bytes_sent_field.store(0, Ordering::Relaxed);
        self.bytes_received_field.store(0, Ordering::Relaxed);
        self.reconnect_count_field.store(0, Ordering::Relaxed);
        self.retry_attempts_field.store(0, Ordering::Relaxed);
        self.queue_dropped_requests_field
            .store(0, Ordering::Relaxed);
        self.circuit_open_rejections_field
            .store(0, Ordering::Relaxed);
        self.circuit_open_transitions_field
            .store(0, Ordering::Relaxed);
        self.circuit_half_open_transitions_field
            .store(0, Ordering::Relaxed);
        self.circuit_closed_transitions_field
            .store(0, Ordering::Relaxed);
        self.connect_time_ms_field.store(0, Ordering::Relaxed);
        self.total_latency_us_field.store(0, Ordering::Relaxed);
        self.min_latency_us_field.store(u64::MAX, Ordering::Relaxed);
        self.max_latency_us_field.store(0, Ordering::Relaxed);
    }

    pub fn decrement_in_flight(&self) {
        let in_flight = self.in_flight_requests_field.load(Ordering::Relaxed);
        if in_flight == 0 {
            return;
        }
        self.in_flight_requests_field
            .store(in_flight - 1, Ordering::Relaxed);
    }
}
