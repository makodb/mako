module;

#include <cstdint>
#include <rusty/cell.hpp>

export module rrr.connection_metrics;

import std;

// @safe - Pure rusty::Cell<uint64_t>-backed counter metrics with simple
// getters/setters. No raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

class ConnectionMetrics {
public:
    ConnectionMetrics() = default;

    uint64_t requests_sent() const { return requests_sent_.get(); }
    uint64_t requests_completed() const { return requests_completed_.get(); }
    uint64_t requests_failed() const { return requests_failed_.get(); }
    uint64_t requests_timed_out() const { return requests_timed_out_.get(); }
    uint64_t in_flight_requests() const { return in_flight_requests_.get(); }

    uint64_t bytes_sent() const { return bytes_sent_.get(); }
    uint64_t bytes_received() const { return bytes_received_.get(); }

    uint64_t reconnect_count() const { return reconnect_count_.get(); }
    uint64_t retry_attempts() const { return retry_attempts_.get(); }
    uint64_t queue_dropped_requests() const { return queue_dropped_requests_.get(); }
    uint64_t circuit_open_rejections() const { return circuit_open_rejections_.get(); }
    uint64_t circuit_open_transitions() const { return circuit_open_transitions_.get(); }
    uint64_t circuit_half_open_transitions() const { return circuit_half_open_transitions_.get(); }
    uint64_t circuit_closed_transitions() const { return circuit_closed_transitions_.get(); }
    uint64_t connect_time_ms() const { return connect_time_ms_.get(); }

    uint64_t min_latency_us() const {
        auto min = min_latency_us_.get();
        return (min == std::numeric_limits<uint64_t>::max()) ? 0 : min;
    }
    uint64_t max_latency_us() const { return max_latency_us_.get(); }

    uint64_t success_rate_percent() const {
        auto completed = requests_completed_.get();
        auto total = requests_sent_.get();
        if (total == 0) return 100;
        return (completed * 100) / total;
    }

    uint64_t avg_latency_us() const {
        auto completed = requests_completed_.get();
        if (completed == 0) return 0;
        return total_latency_us_.get() / completed;
    }

    uint64_t uptime_ms(uint64_t current_time_ms) const {
        auto connect_time = connect_time_ms_.get();
        if (connect_time == 0) return 0;
        if (current_time_ms < connect_time) return 0;
        return current_time_ms - connect_time;
    }

    void record_request_sent() const {
        requests_sent_.set(requests_sent_.get() + 1);
        in_flight_requests_.set(in_flight_requests_.get() + 1);
    }

    void record_request_completed(uint64_t latency_us) const {
        requests_completed_.set(requests_completed_.get() + 1);
        decrement_in_flight();
        total_latency_us_.set(total_latency_us_.get() + latency_us);

        auto current_min = min_latency_us_.get();
        if (latency_us < current_min) {
            min_latency_us_.set(latency_us);
        }

        auto current_max = max_latency_us_.get();
        if (latency_us > current_max) {
            max_latency_us_.set(latency_us);
        }
    }

    void record_request_completed() const {
        requests_completed_.set(requests_completed_.get() + 1);
        decrement_in_flight();
    }

    void record_request_failed() const {
        requests_failed_.set(requests_failed_.get() + 1);
        decrement_in_flight();
    }

    void record_request_timeout() const {
        requests_timed_out_.set(requests_timed_out_.get() + 1);
        decrement_in_flight();
    }

    void record_request_dropped() const {
        decrement_in_flight();
    }

    void record_bytes_sent(uint64_t bytes) const {
        bytes_sent_.set(bytes_sent_.get() + bytes);
    }

    void record_bytes_received(uint64_t bytes) const {
        bytes_received_.set(bytes_received_.get() + bytes);
    }

    void record_reconnect() const {
        reconnect_count_.set(reconnect_count_.get() + 1);
    }

    void record_retry_attempt() const {
        retry_attempts_.set(retry_attempts_.get() + 1);
    }

    void record_queue_drop() const {
        queue_dropped_requests_.set(queue_dropped_requests_.get() + 1);
    }

    void record_circuit_open_rejection() const {
        circuit_open_rejections_.set(circuit_open_rejections_.get() + 1);
    }

    void record_circuit_open_transition() const {
        circuit_open_transitions_.set(circuit_open_transitions_.get() + 1);
    }

    void record_circuit_half_open_transition() const {
        circuit_half_open_transitions_.set(circuit_half_open_transitions_.get() + 1);
    }

    void record_circuit_closed_transition() const {
        circuit_closed_transitions_.set(circuit_closed_transitions_.get() + 1);
    }

    void record_connect(uint64_t current_time_ms) const {
        connect_time_ms_.set(current_time_ms);
    }

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
    mutable rusty::Cell<uint64_t> requests_sent_{0};
    mutable rusty::Cell<uint64_t> requests_completed_{0};
    mutable rusty::Cell<uint64_t> requests_failed_{0};
    mutable rusty::Cell<uint64_t> requests_timed_out_{0};
    mutable rusty::Cell<uint64_t> in_flight_requests_{0};

    mutable rusty::Cell<uint64_t> bytes_sent_{0};
    mutable rusty::Cell<uint64_t> bytes_received_{0};

    mutable rusty::Cell<uint64_t> reconnect_count_{0};
    mutable rusty::Cell<uint64_t> retry_attempts_{0};
    mutable rusty::Cell<uint64_t> queue_dropped_requests_{0};
    mutable rusty::Cell<uint64_t> circuit_open_rejections_{0};
    mutable rusty::Cell<uint64_t> circuit_open_transitions_{0};
    mutable rusty::Cell<uint64_t> circuit_half_open_transitions_{0};
    mutable rusty::Cell<uint64_t> circuit_closed_transitions_{0};
    mutable rusty::Cell<uint64_t> connect_time_ms_{0};

    mutable rusty::Cell<uint64_t> total_latency_us_{0};
    mutable rusty::Cell<uint64_t> min_latency_us_{std::numeric_limits<uint64_t>::max()};
    mutable rusty::Cell<uint64_t> max_latency_us_{0};

    void decrement_in_flight() const {
        auto in_flight = in_flight_requests_.get();
        if (in_flight == 0) {
            return;
        }
        in_flight_requests_.set(in_flight - 1);
    }
};

}  // export namespace rrr
