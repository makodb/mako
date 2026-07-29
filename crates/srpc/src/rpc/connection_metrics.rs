//! Per-connection counters — the port of
//! `src/rrr/rpc/connection_metrics.cpp`.
//!
//! Supplies exactly what [`crate::rpc::load_balancer::Candidate`] needs,
//! so a connection's metrics can drive peer selection directly.
//!
//! ## One deliberate behaviour change
//!
//! The C++ increments read `load(Relaxed)`, add one, then
//! `store(Relaxed)` — a read-modify-write that is **not atomic** even
//! though the fields are atomics. Two threads completing requests at
//! the same moment can read the same value and store the same result,
//! losing a count. This port uses `fetch_add`/`fetch_sub` and
//! `fetch_min`/`fetch_max`, which are genuinely atomic. The counters
//! are still `Relaxed`: they are statistics, not synchronisation, so no
//! ordering relationship with other data is implied or needed.
//!
//! In-flight decrements saturate at zero. A stray completion must not
//! wrap the gauge to `u64::MAX` and make the connection look infinitely
//! busy to the load balancer.

use super::circuit_breaker::CircuitState;
use super::load_balancer::Candidate;
use std::sync::atomic::{AtomicU64, Ordering};

/// Sentinel for "no latency recorded yet", so the first sample always
/// wins the minimum. Reported as `0` by [`ConnectionMetrics::min_latency_us`].
const NO_MIN: u64 = u64::MAX;

#[derive(Debug, Default)]
pub struct ConnectionMetrics {
    requests_sent: AtomicU64,
    requests_completed: AtomicU64,
    requests_failed: AtomicU64,
    requests_timed_out: AtomicU64,
    in_flight: AtomicU64,

    bytes_sent: AtomicU64,
    bytes_received: AtomicU64,

    reconnect_count: AtomicU64,
    retry_attempts: AtomicU64,
    queue_dropped_requests: AtomicU64,

    circuit_open_rejections: AtomicU64,
    circuit_open_transitions: AtomicU64,
    circuit_half_open_transitions: AtomicU64,
    circuit_closed_transitions: AtomicU64,

    connect_time_ms: AtomicU64,
    total_latency_us: AtomicU64,
    min_latency_us: AtomicU64,
    max_latency_us: AtomicU64,
}

impl ConnectionMetrics {
    pub fn new() -> ConnectionMetrics {
        ConnectionMetrics {
            min_latency_us: AtomicU64::new(NO_MIN),
            ..Default::default()
        }
    }

    // ---- readers ---------------------------------------------------

    pub fn requests_sent(&self) -> u64 {
        self.requests_sent.load(Ordering::Relaxed)
    }
    pub fn requests_completed(&self) -> u64 {
        self.requests_completed.load(Ordering::Relaxed)
    }
    pub fn requests_failed(&self) -> u64 {
        self.requests_failed.load(Ordering::Relaxed)
    }
    pub fn requests_timed_out(&self) -> u64 {
        self.requests_timed_out.load(Ordering::Relaxed)
    }
    pub fn in_flight_requests(&self) -> u64 {
        self.in_flight.load(Ordering::Relaxed)
    }
    pub fn bytes_sent(&self) -> u64 {
        self.bytes_sent.load(Ordering::Relaxed)
    }
    pub fn bytes_received(&self) -> u64 {
        self.bytes_received.load(Ordering::Relaxed)
    }
    pub fn reconnect_count(&self) -> u64 {
        self.reconnect_count.load(Ordering::Relaxed)
    }
    pub fn retry_attempts(&self) -> u64 {
        self.retry_attempts.load(Ordering::Relaxed)
    }
    pub fn queue_dropped_requests(&self) -> u64 {
        self.queue_dropped_requests.load(Ordering::Relaxed)
    }
    pub fn circuit_open_rejections(&self) -> u64 {
        self.circuit_open_rejections.load(Ordering::Relaxed)
    }
    pub fn circuit_open_transitions(&self) -> u64 {
        self.circuit_open_transitions.load(Ordering::Relaxed)
    }
    pub fn circuit_half_open_transitions(&self) -> u64 {
        self.circuit_half_open_transitions.load(Ordering::Relaxed)
    }
    pub fn circuit_closed_transitions(&self) -> u64 {
        self.circuit_closed_transitions.load(Ordering::Relaxed)
    }
    pub fn connect_time_ms(&self) -> u64 {
        self.connect_time_ms.load(Ordering::Relaxed)
    }

    /// Fastest completed request, or `0` before any completes.
    pub fn min_latency_us(&self) -> u64 {
        let min = self.min_latency_us.load(Ordering::Relaxed);
        if min == NO_MIN {
            0
        } else {
            min
        }
    }

    pub fn max_latency_us(&self) -> u64 {
        self.max_latency_us.load(Ordering::Relaxed)
    }

    /// Mean latency of completed requests, or `0` if none completed.
    pub fn avg_latency_us(&self) -> u64 {
        let completed = self.requests_completed.load(Ordering::Relaxed);
        if completed == 0 {
            return 0;
        }
        self.total_latency_us.load(Ordering::Relaxed) / completed
    }

    /// Completed as a percentage of sent. A connection that has sent
    /// nothing reports 100: no evidence of failure, and reporting 0
    /// would make an idle peer look broken to anything reading this.
    pub fn success_rate_percent(&self) -> u64 {
        let total = self.requests_sent.load(Ordering::Relaxed);
        if total == 0 {
            return 100;
        }
        (self.requests_completed.load(Ordering::Relaxed) * 100) / total
    }

    /// Milliseconds since connect. `0` if never connected, or if the
    /// supplied clock reading precedes the connect stamp.
    pub fn uptime_ms(&self, current_time_ms: u64) -> u64 {
        let connected = self.connect_time_ms.load(Ordering::Relaxed);
        if connected == 0 || current_time_ms < connected {
            return 0;
        }
        current_time_ms - connected
    }

    // ---- recorders -------------------------------------------------

    pub fn record_request_sent(&self) {
        self.requests_sent.fetch_add(1, Ordering::Relaxed);
        self.in_flight.fetch_add(1, Ordering::Relaxed);
    }

    /// Decrement the in-flight gauge without wrapping. A stray
    /// completion would otherwise leave the connection looking
    /// infinitely busy.
    fn release_in_flight(&self) {
        let _ = self
            .in_flight
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |v| {
                Some(v.saturating_sub(1))
            });
    }

    pub fn record_request_completed(&self) {
        self.requests_completed.fetch_add(1, Ordering::Relaxed);
        self.release_in_flight();
    }

    pub fn record_request_completed_with_latency(&self, latency_us: u64) {
        self.requests_completed.fetch_add(1, Ordering::Relaxed);
        self.release_in_flight();
        self.total_latency_us
            .fetch_add(latency_us, Ordering::Relaxed);
        self.min_latency_us.fetch_min(latency_us, Ordering::Relaxed);
        self.max_latency_us.fetch_max(latency_us, Ordering::Relaxed);
    }

    pub fn record_request_failed(&self) {
        self.requests_failed.fetch_add(1, Ordering::Relaxed);
        self.release_in_flight();
    }

    pub fn record_request_timeout(&self) {
        self.requests_timed_out.fetch_add(1, Ordering::Relaxed);
        self.release_in_flight();
    }

    /// A request dropped before it was ever sent (queue overflow), so
    /// there is no in-flight entry to release.
    pub fn record_request_dropped(&self) {
        self.queue_dropped_requests.fetch_add(1, Ordering::Relaxed);
    }

    pub fn record_retry_attempt(&self) {
        self.retry_attempts.fetch_add(1, Ordering::Relaxed);
    }

    pub fn record_bytes_sent(&self, n: u64) {
        self.bytes_sent.fetch_add(n, Ordering::Relaxed);
    }

    pub fn record_bytes_received(&self, n: u64) {
        self.bytes_received.fetch_add(n, Ordering::Relaxed);
    }

    pub fn record_reconnect(&self) {
        self.reconnect_count.fetch_add(1, Ordering::Relaxed);
    }

    pub fn record_circuit_open_rejection(&self) {
        self.circuit_open_rejections.fetch_add(1, Ordering::Relaxed);
    }

    pub fn record_circuit_transition(&self, to: CircuitState) {
        let counter = match to {
            CircuitState::Open => &self.circuit_open_transitions,
            CircuitState::HalfOpen => &self.circuit_half_open_transitions,
            CircuitState::Closed => &self.circuit_closed_transitions,
        };
        counter.fetch_add(1, Ordering::Relaxed);
    }

    pub fn set_connect_time_ms(&self, t: u64) {
        self.connect_time_ms.store(t, Ordering::Relaxed);
    }

    /// Zero every counter, as on a reconnect.
    ///
    /// Written out rather than looped over a slice of references: a
    /// `&[&AtomicU64]` lowers to `reference_wrapper`, which does not
    /// auto-deref to the atomic's methods in C++.
    pub fn reset(&self) {
        let z = Ordering::Relaxed;
        self.requests_sent.store(0, z);
        self.requests_completed.store(0, z);
        self.requests_failed.store(0, z);
        self.requests_timed_out.store(0, z);
        self.in_flight.store(0, z);
        self.bytes_sent.store(0, z);
        self.bytes_received.store(0, z);
        self.reconnect_count.store(0, z);
        self.retry_attempts.store(0, z);
        self.queue_dropped_requests.store(0, z);
        self.circuit_open_rejections.store(0, z);
        self.circuit_open_transitions.store(0, z);
        self.circuit_half_open_transitions.store(0, z);
        self.circuit_closed_transitions.store(0, z);
        self.connect_time_ms.store(0, z);
        self.total_latency_us.store(0, z);
        self.max_latency_us.store(0, z);
        self.min_latency_us.store(NO_MIN, z);
    }
}

impl Candidate for ConnectionMetrics {
    fn in_flight_requests(&self) -> u64 {
        ConnectionMetrics::in_flight_requests(self)
    }
    fn avg_latency_us(&self) -> u64 {
        ConnectionMetrics::avg_latency_us(self)
    }
    fn requests_completed(&self) -> u64 {
        ConnectionMetrics::requests_completed(self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn a_fresh_connection_reports_sensible_zeros() {
        let m = ConnectionMetrics::new();
        assert_eq!(m.requests_sent(), 0);
        assert_eq!(m.in_flight_requests(), 0);
        assert_eq!(m.avg_latency_us(), 0, "no completions, no average");
        assert_eq!(m.min_latency_us(), 0, "the sentinel is not exposed");
        assert_eq!(m.max_latency_us(), 0);
        assert_eq!(
            m.success_rate_percent(),
            100,
            "an idle peer is not a failing one"
        );
        assert_eq!(m.uptime_ms(1_000_000), 0, "never connected");
    }

    #[test]
    fn send_and_complete_move_the_in_flight_gauge() {
        let m = ConnectionMetrics::new();
        m.record_request_sent();
        m.record_request_sent();
        assert_eq!(m.requests_sent(), 2);
        assert_eq!(m.in_flight_requests(), 2);

        m.record_request_completed();
        assert_eq!(m.in_flight_requests(), 1);
        m.record_request_failed();
        assert_eq!(m.in_flight_requests(), 0);
        assert_eq!(m.requests_completed(), 1);
        assert_eq!(m.requests_failed(), 1);
    }

    #[test]
    fn in_flight_saturates_instead_of_wrapping() {
        // A stray completion must not make the connection look
        // infinitely busy to the load balancer.
        let m = ConnectionMetrics::new();
        m.record_request_completed();
        m.record_request_timeout();
        assert_eq!(m.in_flight_requests(), 0, "must not wrap to u64::MAX");
    }

    #[test]
    fn dropped_requests_do_not_touch_the_gauge() {
        // A queue-overflow drop happens before the request is sent.
        let m = ConnectionMetrics::new();
        m.record_request_sent();
        m.record_request_dropped();
        assert_eq!(m.in_flight_requests(), 1);
        assert_eq!(m.queue_dropped_requests(), 1);
    }

    #[test]
    fn latency_tracks_min_max_and_mean() {
        let m = ConnectionMetrics::new();
        m.record_request_sent();
        m.record_request_completed_with_latency(300);
        assert_eq!(m.min_latency_us(), 300, "first sample wins the minimum");
        assert_eq!(m.max_latency_us(), 300);
        assert_eq!(m.avg_latency_us(), 300);

        m.record_request_sent();
        m.record_request_completed_with_latency(100);
        m.record_request_sent();
        m.record_request_completed_with_latency(500);
        assert_eq!(m.min_latency_us(), 100);
        assert_eq!(m.max_latency_us(), 500);
        assert_eq!(m.avg_latency_us(), 300, "(300+100+500)/3");
    }

    #[test]
    fn a_zero_latency_sample_is_recorded_not_ignored() {
        let m = ConnectionMetrics::new();
        m.record_request_sent();
        m.record_request_completed_with_latency(0);
        assert_eq!(m.min_latency_us(), 0);
        assert_eq!(m.requests_completed(), 1, "and it counts as completed");
    }

    #[test]
    fn success_rate_and_uptime() {
        let m = ConnectionMetrics::new();
        let mut i = 0;
        while i < 4 {
            m.record_request_sent();
            i += 1;
        }
        m.record_request_completed();
        m.record_request_completed();
        m.record_request_completed();
        assert_eq!(m.success_rate_percent(), 75);

        m.set_connect_time_ms(1_000);
        assert_eq!(m.uptime_ms(3_500), 2_500);
        assert_eq!(m.uptime_ms(500), 0, "a clock reading before connect");
    }

    #[test]
    fn byte_and_lifecycle_counters() {
        let m = ConnectionMetrics::new();
        m.record_bytes_sent(1200);
        m.record_bytes_received(340);
        m.record_bytes_sent(800);
        assert_eq!(m.bytes_sent(), 2000);
        assert_eq!(m.bytes_received(), 340);

        m.record_reconnect();
        m.record_retry_attempt();
        m.record_retry_attempt();
        m.record_circuit_open_rejection();
        assert_eq!(m.reconnect_count(), 1);
        assert_eq!(m.retry_attempts(), 2);
        assert_eq!(m.circuit_open_rejections(), 1);
    }

    #[test]
    fn circuit_transitions_count_per_state() {
        let m = ConnectionMetrics::new();
        m.record_circuit_transition(CircuitState::Open);
        m.record_circuit_transition(CircuitState::HalfOpen);
        m.record_circuit_transition(CircuitState::Open);
        m.record_circuit_transition(CircuitState::Closed);
        assert_eq!(m.circuit_open_transitions(), 2);
        assert_eq!(m.circuit_half_open_transitions(), 1);
        assert_eq!(m.circuit_closed_transitions(), 1);
    }

    #[test]
    fn reset_restores_a_fresh_connection() {
        let m = ConnectionMetrics::new();
        m.record_request_sent();
        m.record_request_completed_with_latency(250);
        m.record_bytes_sent(99);
        m.set_connect_time_ms(5);
        m.reset();
        assert_eq!(m.requests_sent(), 0);
        assert_eq!(m.bytes_sent(), 0);
        assert_eq!(m.min_latency_us(), 0, "sentinel restored");
        assert_eq!(m.max_latency_us(), 0);
        assert_eq!(m.avg_latency_us(), 0);
        assert_eq!(m.success_rate_percent(), 100);
    }

    /// The C++ increments are load-then-store, which loses counts when
    /// two threads race. `fetch_add` does not, and this is the test
    /// that would catch a regression back to the old shape.
    #[test]
    fn concurrent_recording_loses_nothing() {
        let m = Arc::new(ConnectionMetrics::new());
        let mut handles = Vec::new();
        let threads = 4;
        let per_thread = 5000;
        let mut t = 0;
        while t < threads {
            let m = Arc::clone(&m);
            handles.push(std::thread::spawn(move || {
                let mut i = 0;
                while i < per_thread {
                    m.record_request_sent();
                    m.record_request_completed_with_latency(10);
                    m.record_bytes_sent(2);
                    i += 1;
                }
            }));
            t += 1;
        }
        for h in handles {
            h.join().unwrap();
        }
        let total = threads * per_thread;
        assert_eq!(m.requests_sent(), total);
        assert_eq!(m.requests_completed(), total);
        assert_eq!(m.bytes_sent(), total * 2);
        assert_eq!(m.in_flight_requests(), 0, "every send was released");
        assert_eq!(m.avg_latency_us(), 10);
    }

    #[test]
    fn metrics_drive_the_load_balancer() {
        use crate::base::rand::Rng;
        use crate::rpc::load_balancer::{LoadBalancer, LoadBalancerState, LoadBalancingStrategy};

        let busy = ConnectionMetrics::new();
        busy.record_request_sent();
        busy.record_request_sent();
        busy.record_request_sent();

        let idle = ConnectionMetrics::new();
        idle.record_request_sent();
        idle.record_request_completed_with_latency(50);

        let peers = [busy, idle];
        let st = LoadBalancerState::new();
        let rng = Rng::with_seed(1);
        assert_eq!(
            LoadBalancer::select(LoadBalancingStrategy::LeastConnections, &peers, &st, &rng),
            Some(1),
            "the idle connection wins"
        );
        assert_eq!(
            LoadBalancer::select(LoadBalancingStrategy::LeastLatency, &peers, &st, &rng),
            Some(1),
            "and it is the only one with a measurement"
        );
    }
}
