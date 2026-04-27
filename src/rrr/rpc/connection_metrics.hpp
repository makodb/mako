#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

/**
 * Connection Health Metrics (Phase 3.2)
 *
 * Tracks request, data transfer, connection lifecycle, and latency metrics
 * for monitoring connection health and performance.
 *
 * All metrics are thread-safe via rusty::Cell.
 *
 * Note: Time-related methods accept timestamps as parameters to avoid
 * internal std::chrono dependencies. Callers should provide timestamps.
 */

#include <rusty/cell.hpp>




namespace rrr {

// @safe - All fields use Cell for thread-safe interior mutability
class ConnectionMetrics {
public:
    // @safe - Default constructor
    ConnectionMetrics() = default;

    // === Request Counters ===

    // @safe - Get number of requests sent
    uint64_t requests_sent() const {
        return requests_sent_.get();
    }

    // @safe - Get number of requests completed successfully
    uint64_t requests_completed() const {
        return requests_completed_.get();
    }

    // @safe - Get number of requests that failed
    uint64_t requests_failed() const {
        return requests_failed_.get();
    }

    // @safe - Get number of requests that timed out
    uint64_t requests_timed_out() const {
        return requests_timed_out_.get();
    }

    // @safe - Get number of currently in-flight requests.
    uint64_t in_flight_requests() const {
        return in_flight_requests_.get();
    }

    // === Data Transfer Counters ===

    // @safe - Get total bytes sent
    uint64_t bytes_sent() const {
        return bytes_sent_.get();
    }

    // @safe - Get total bytes received
    uint64_t bytes_received() const {
        return bytes_received_.get();
    }

    // === Connection Lifecycle ===

    // @safe - Get number of reconnection attempts
    uint64_t reconnect_count() const {
        return reconnect_count_.get();
    }

    // @safe - Get number of retry attempts executed
    uint64_t retry_attempts() const {
        return retry_attempts_.get();
    }

    // @safe - Get number of requests dropped by queue policy or expiry.
    uint64_t queue_dropped_requests() const {
        return queue_dropped_requests_.get();
    }

    // @safe - Get number of requests rejected due to open circuit.
    uint64_t circuit_open_rejections() const {
        return circuit_open_rejections_.get();
    }

    // @safe - Get number of transitions into OPEN state.
    uint64_t circuit_open_transitions() const {
        return circuit_open_transitions_.get();
    }

    // @safe - Get number of transitions into HALF_OPEN state.
    uint64_t circuit_half_open_transitions() const {
        return circuit_half_open_transitions_.get();
    }

    // @safe - Get number of transitions into CLOSED state.
    uint64_t circuit_closed_transitions() const {
        return circuit_closed_transitions_.get();
    }

    // @safe - Get timestamp when connection was established (ms since epoch)
    uint64_t connect_time_ms() const {
        return connect_time_ms_.get();
    }

    // === Latency Metrics ===

    // @safe - Get minimum latency in microseconds
    uint64_t min_latency_us() const {
        auto min = min_latency_us_.get();
        return (min == std::numeric_limits<uint64_t>::max()) ? 0 : min;
    }

    // @safe - Get maximum latency in microseconds
    uint64_t max_latency_us() const {
        return max_latency_us_.get();
    }

    // === Computed Metrics ===

    // @safe - Calculate success rate as percentage (0-100)
    uint64_t success_rate_percent() const {
        auto completed = requests_completed_.get();
        auto total = requests_sent_.get();
        if (total == 0) return 100;  // No requests = 100% success
        return (completed * 100) / total;
    }

    // @safe - Calculate average latency in microseconds
    uint64_t avg_latency_us() const {
        auto completed = requests_completed_.get();
        if (completed == 0) return 0;
        return total_latency_us_.get() / completed;
    }

    // @safe - Calculate connection uptime in milliseconds
    // @param current_time_ms Current time in milliseconds since epoch
    uint64_t uptime_ms(uint64_t current_time_ms) const {
        auto connect_time = connect_time_ms_.get();
        if (connect_time == 0) return 0;
        if (current_time_ms < connect_time) return 0;
        return current_time_ms - connect_time;
    }

    // === Recording Methods ===

    // @safe - Record that a request was sent
    void record_request_sent() const {
        requests_sent_.set(requests_sent_.get() + 1);
        in_flight_requests_.set(in_flight_requests_.get() + 1);
    }

    // @safe - Record that a request completed successfully with latency
    void record_request_completed(uint64_t latency_us) const {
        requests_completed_.set(requests_completed_.get() + 1);
        decrement_in_flight();
        total_latency_us_.set(total_latency_us_.get() + latency_us);

        // Update min/max
        auto current_min = min_latency_us_.get();
        if (latency_us < current_min) {
            min_latency_us_.set(latency_us);
        }

        auto current_max = max_latency_us_.get();
        if (latency_us > current_max) {
            max_latency_us_.set(latency_us);
        }
    }

    // @safe - Record that a request completed (without latency tracking)
    void record_request_completed() const {
        requests_completed_.set(requests_completed_.get() + 1);
        decrement_in_flight();
    }

    // @safe - Record that a request failed
    void record_request_failed() const {
        requests_failed_.set(requests_failed_.get() + 1);
        decrement_in_flight();
    }

    // @safe - Record that a request timed out
    void record_request_timeout() const {
        requests_timed_out_.set(requests_timed_out_.get() + 1);
        decrement_in_flight();
    }

    // @safe - Record that an in-flight request was dropped/cancelled.
    void record_request_dropped() const {
        decrement_in_flight();
    }

    // @safe - Record bytes sent
    void record_bytes_sent(uint64_t bytes) const {
        bytes_sent_.set(bytes_sent_.get() + bytes);
    }

    // @safe - Record bytes received
    void record_bytes_received(uint64_t bytes) const {
        bytes_received_.set(bytes_received_.get() + bytes);
    }

    // @safe - Record a reconnection attempt
    void record_reconnect() const {
        reconnect_count_.set(reconnect_count_.get() + 1);
    }

    // @safe - Record a retry attempt
    void record_retry_attempt() const {
        retry_attempts_.set(retry_attempts_.get() + 1);
    }

    // @safe - Record request drop from queue overflow/expiry policy.
    void record_queue_drop() const {
        queue_dropped_requests_.set(queue_dropped_requests_.get() + 1);
    }

    // @safe - Record circuit fail-fast rejection.
    void record_circuit_open_rejection() const {
        circuit_open_rejections_.set(circuit_open_rejections_.get() + 1);
    }

    // @safe - Record transition into OPEN state.
    void record_circuit_open_transition() const {
        circuit_open_transitions_.set(circuit_open_transitions_.get() + 1);
    }

    // @safe - Record transition into HALF_OPEN state.
    void record_circuit_half_open_transition() const {
        circuit_half_open_transitions_.set(circuit_half_open_transitions_.get() + 1);
    }

    // @safe - Record transition into CLOSED state.
    void record_circuit_closed_transition() const {
        circuit_closed_transitions_.set(circuit_closed_transitions_.get() + 1);
    }

    // @safe - Record connection established
    // @param current_time_ms Current time in milliseconds since epoch
    void record_connect(uint64_t current_time_ms) const {
        connect_time_ms_.set(current_time_ms);
    }

    // @safe - Reset all metrics to initial values
    void reset() const {
        requests_sent_.set(0);
        requests_completed_.set(0);
        requests_failed_.set(0);
        requests_timed_out_.set(0);
        in_flight_requests_.set(0);
        bytes_sent_.set(0);
        bytes_received_.set(0);
        reconnect_count_.set(0);
        retry_attempts_.set(0);
        queue_dropped_requests_.set(0);
        circuit_open_rejections_.set(0);
        circuit_open_transitions_.set(0);
        circuit_half_open_transitions_.set(0);
        circuit_closed_transitions_.set(0);
        connect_time_ms_.set(0);
        total_latency_us_.set(0);
        min_latency_us_.set(std::numeric_limits<uint64_t>::max());
        max_latency_us_.set(0);
    }

private:
    // Request counters
    mutable rusty::Cell<uint64_t> requests_sent_{0};
    mutable rusty::Cell<uint64_t> requests_completed_{0};
    mutable rusty::Cell<uint64_t> requests_failed_{0};
    mutable rusty::Cell<uint64_t> requests_timed_out_{0};
    mutable rusty::Cell<uint64_t> in_flight_requests_{0};

    // Data transfer counters
    mutable rusty::Cell<uint64_t> bytes_sent_{0};
    mutable rusty::Cell<uint64_t> bytes_received_{0};

    // Connection lifecycle
    mutable rusty::Cell<uint64_t> reconnect_count_{0};
    mutable rusty::Cell<uint64_t> retry_attempts_{0};
    mutable rusty::Cell<uint64_t> queue_dropped_requests_{0};
    mutable rusty::Cell<uint64_t> circuit_open_rejections_{0};
    mutable rusty::Cell<uint64_t> circuit_open_transitions_{0};
    mutable rusty::Cell<uint64_t> circuit_half_open_transitions_{0};
    mutable rusty::Cell<uint64_t> circuit_closed_transitions_{0};
    mutable rusty::Cell<uint64_t> connect_time_ms_{0};

    // Latency tracking (microseconds)
    mutable rusty::Cell<uint64_t> total_latency_us_{0};
    mutable rusty::Cell<uint64_t> min_latency_us_{std::numeric_limits<uint64_t>::max()};
    mutable rusty::Cell<uint64_t> max_latency_us_{0};

    // @safe - Saturating decrement for in-flight counter.
    void decrement_in_flight() const {
        auto in_flight = in_flight_requests_.get();
        if (in_flight == 0) {
            return;
        }
        in_flight_requests_.set(in_flight - 1);
    }
};

}  // namespace rrr
