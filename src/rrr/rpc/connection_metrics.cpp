module;

#include <rusty/cell.hpp>
#include <rusty/move.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>
#include <cstdint>

export module rrr.connection_metrics;

import std;

// @safe - Pure rusty::Cell<uint64_t>-backed counter metrics with simple
// getters/setters. No raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

// `ConnectionMetrics` — bag of `rusty::Cell<u64>` counters. Every
// field is interior-mutable, so every method is `const` and `&self`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The constructor uses the
// `#[cpp_ctor]` attribute so every existing call site
// (`ConnectionMetrics metrics;` in tests, member-init in client.cpp)
// keeps compiling.
//
// Behavioral diffs from the original C++ class:
//   * Fields are no longer marked `private`. No callers reach into
//     them. The trailing `_` on each field name is replaced with
//     `_field` because the transpiler considers e.g.
//     `requests_sent_` to collide with the `requests_sent()`
//     accessor; the rename moves the field out of the way and keeps
//     the public method name unchanged.
//   * The `= default` default ctor becomes a real ctor body
//     (`ConnectionMetrics()`) emitted by `#[cpp_ctor]`. Same effect:
//     all fields are still default-initialized to 0 (except
//     `min_latency_us_field`, which starts at `u64::MAX`).
#if RUSTYCPP_RUST
struct ConnectionMetrics {
    requests_sent_field: rusty::Cell<u64>,
    requests_completed_field: rusty::Cell<u64>,
    requests_failed_field: rusty::Cell<u64>,
    requests_timed_out_field: rusty::Cell<u64>,
    in_flight_requests_field: rusty::Cell<u64>,

    bytes_sent_field: rusty::Cell<u64>,
    bytes_received_field: rusty::Cell<u64>,

    reconnect_count_field: rusty::Cell<u64>,
    retry_attempts_field: rusty::Cell<u64>,
    queue_dropped_requests_field: rusty::Cell<u64>,
    circuit_open_rejections_field: rusty::Cell<u64>,
    circuit_open_transitions_field: rusty::Cell<u64>,
    circuit_half_open_transitions_field: rusty::Cell<u64>,
    circuit_closed_transitions_field: rusty::Cell<u64>,
    connect_time_ms_field: rusty::Cell<u64>,

    total_latency_us_field: rusty::Cell<u64>,
    min_latency_us_field: rusty::Cell<u64>,
    max_latency_us_field: rusty::Cell<u64>,
}

impl ConnectionMetrics {
    #[cpp_ctor]
    fn new() -> ConnectionMetrics {
        ConnectionMetrics {
            requests_sent_field: rusty::Cell::<u64>::new(0u64),
            requests_completed_field: rusty::Cell::<u64>::new(0u64),
            requests_failed_field: rusty::Cell::<u64>::new(0u64),
            requests_timed_out_field: rusty::Cell::<u64>::new(0u64),
            in_flight_requests_field: rusty::Cell::<u64>::new(0u64),
            bytes_sent_field: rusty::Cell::<u64>::new(0u64),
            bytes_received_field: rusty::Cell::<u64>::new(0u64),
            reconnect_count_field: rusty::Cell::<u64>::new(0u64),
            retry_attempts_field: rusty::Cell::<u64>::new(0u64),
            queue_dropped_requests_field: rusty::Cell::<u64>::new(0u64),
            circuit_open_rejections_field: rusty::Cell::<u64>::new(0u64),
            circuit_open_transitions_field: rusty::Cell::<u64>::new(0u64),
            circuit_half_open_transitions_field: rusty::Cell::<u64>::new(0u64),
            circuit_closed_transitions_field: rusty::Cell::<u64>::new(0u64),
            connect_time_ms_field: rusty::Cell::<u64>::new(0u64),
            total_latency_us_field: rusty::Cell::<u64>::new(0u64),
            min_latency_us_field: rusty::Cell::<u64>::new(u64::MAX),
            max_latency_us_field: rusty::Cell::<u64>::new(0u64),
        }
    }

    fn requests_sent(&self) -> u64 { self.requests_sent_field.get() }
    fn requests_completed(&self) -> u64 { self.requests_completed_field.get() }
    fn requests_failed(&self) -> u64 { self.requests_failed_field.get() }
    fn requests_timed_out(&self) -> u64 { self.requests_timed_out_field.get() }
    fn in_flight_requests(&self) -> u64 { self.in_flight_requests_field.get() }

    fn bytes_sent(&self) -> u64 { self.bytes_sent_field.get() }
    fn bytes_received(&self) -> u64 { self.bytes_received_field.get() }

    fn reconnect_count(&self) -> u64 { self.reconnect_count_field.get() }
    fn retry_attempts(&self) -> u64 { self.retry_attempts_field.get() }
    fn queue_dropped_requests(&self) -> u64 { self.queue_dropped_requests_field.get() }
    fn circuit_open_rejections(&self) -> u64 { self.circuit_open_rejections_field.get() }
    fn circuit_open_transitions(&self) -> u64 { self.circuit_open_transitions_field.get() }
    fn circuit_half_open_transitions(&self) -> u64 { self.circuit_half_open_transitions_field.get() }
    fn circuit_closed_transitions(&self) -> u64 { self.circuit_closed_transitions_field.get() }
    fn connect_time_ms(&self) -> u64 { self.connect_time_ms_field.get() }

    fn min_latency_us(&self) -> u64 {
        let min: u64 = self.min_latency_us_field.get();
        if min == u64::MAX { 0u64 } else { min }
    }
    fn max_latency_us(&self) -> u64 { self.max_latency_us_field.get() }

    fn success_rate_percent(&self) -> u64 {
        let completed: u64 = self.requests_completed_field.get();
        let total: u64 = self.requests_sent_field.get();
        if total == 0u64 {
            return 100u64;
        }
        (completed * 100u64) / total
    }

    fn avg_latency_us(&self) -> u64 {
        let completed: u64 = self.requests_completed_field.get();
        if completed == 0u64 {
            return 0u64;
        }
        self.total_latency_us_field.get() / completed
    }

    fn uptime_ms(&self, current_time_ms: u64) -> u64 {
        let connect_time: u64 = self.connect_time_ms_field.get();
        if connect_time == 0u64 {
            return 0u64;
        }
        if current_time_ms < connect_time {
            return 0u64;
        }
        current_time_ms - connect_time
    }

    fn record_request_sent(&self) {
        self.requests_sent_field.set(self.requests_sent_field.get() + 1u64);
        self.in_flight_requests_field.set(self.in_flight_requests_field.get() + 1u64);
    }

    fn record_request_completed_with_latency(&self, latency_us: u64) {
        self.requests_completed_field.set(self.requests_completed_field.get() + 1u64);
        self.decrement_in_flight();
        self.total_latency_us_field.set(self.total_latency_us_field.get() + latency_us);

        let current_min: u64 = self.min_latency_us_field.get();
        if latency_us < current_min {
            self.min_latency_us_field.set(latency_us);
        }

        let current_max: u64 = self.max_latency_us_field.get();
        if latency_us > current_max {
            self.max_latency_us_field.set(latency_us);
        }
    }

    fn record_request_completed(&self) {
        self.requests_completed_field.set(self.requests_completed_field.get() + 1u64);
        self.decrement_in_flight();
    }

    fn record_request_failed(&self) {
        self.requests_failed_field.set(self.requests_failed_field.get() + 1u64);
        self.decrement_in_flight();
    }

    fn record_request_timeout(&self) {
        self.requests_timed_out_field.set(self.requests_timed_out_field.get() + 1u64);
        self.decrement_in_flight();
    }

    fn record_request_dropped(&self) {
        self.decrement_in_flight();
    }

    fn record_bytes_sent(&self, bytes: u64) {
        self.bytes_sent_field.set(self.bytes_sent_field.get() + bytes);
    }

    fn record_bytes_received(&self, bytes: u64) {
        self.bytes_received_field.set(self.bytes_received_field.get() + bytes);
    }

    fn record_reconnect(&self) {
        self.reconnect_count_field.set(self.reconnect_count_field.get() + 1u64);
    }

    fn record_retry_attempt(&self) {
        self.retry_attempts_field.set(self.retry_attempts_field.get() + 1u64);
    }

    fn record_queue_drop(&self) {
        self.queue_dropped_requests_field.set(self.queue_dropped_requests_field.get() + 1u64);
    }

    fn record_circuit_open_rejection(&self) {
        self.circuit_open_rejections_field.set(self.circuit_open_rejections_field.get() + 1u64);
    }

    fn record_circuit_open_transition(&self) {
        self.circuit_open_transitions_field.set(self.circuit_open_transitions_field.get() + 1u64);
    }

    fn record_circuit_half_open_transition(&self) {
        self.circuit_half_open_transitions_field.set(self.circuit_half_open_transitions_field.get() + 1u64);
    }

    fn record_circuit_closed_transition(&self) {
        self.circuit_closed_transitions_field.set(self.circuit_closed_transitions_field.get() + 1u64);
    }

    fn record_connect(&self, current_time_ms: u64) {
        self.connect_time_ms_field.set(current_time_ms);
    }

    fn reset(&self) {
        self.requests_sent_field.set(0u64);
        self.requests_completed_field.set(0u64);
        self.requests_failed_field.set(0u64);
        self.requests_timed_out_field.set(0u64);
        self.in_flight_requests_field.set(0u64);
        self.bytes_sent_field.set(0u64);
        self.bytes_received_field.set(0u64);
        self.reconnect_count_field.set(0u64);
        self.retry_attempts_field.set(0u64);
        self.queue_dropped_requests_field.set(0u64);
        self.circuit_open_rejections_field.set(0u64);
        self.circuit_open_transitions_field.set(0u64);
        self.circuit_half_open_transitions_field.set(0u64);
        self.circuit_closed_transitions_field.set(0u64);
        self.connect_time_ms_field.set(0u64);
        self.total_latency_us_field.set(0u64);
        self.min_latency_us_field.set(u64::MAX);
        self.max_latency_us_field.set(0u64);
    }

    fn decrement_in_flight(&self) {
        let in_flight: u64 = self.in_flight_requests_field.get();
        if in_flight == 0u64 {
            return;
        }
        self.in_flight_requests_field.set(in_flight - 1u64);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=connection_metrics.1 version=1 rust_sha256=9867e742531d7b909fcd032e67e221f169689223060d1ef1ad45f85e779cb0cc*/
struct ConnectionMetrics;

struct ConnectionMetrics {
    rusty::Cell<uint64_t> requests_sent_field;
    rusty::Cell<uint64_t> requests_completed_field;
    rusty::Cell<uint64_t> requests_failed_field;
    rusty::Cell<uint64_t> requests_timed_out_field;
    rusty::Cell<uint64_t> in_flight_requests_field;
    rusty::Cell<uint64_t> bytes_sent_field;
    rusty::Cell<uint64_t> bytes_received_field;
    rusty::Cell<uint64_t> reconnect_count_field;
    rusty::Cell<uint64_t> retry_attempts_field;
    rusty::Cell<uint64_t> queue_dropped_requests_field;
    rusty::Cell<uint64_t> circuit_open_rejections_field;
    rusty::Cell<uint64_t> circuit_open_transitions_field;
    rusty::Cell<uint64_t> circuit_half_open_transitions_field;
    rusty::Cell<uint64_t> circuit_closed_transitions_field;
    rusty::Cell<uint64_t> connect_time_ms_field;
    rusty::Cell<uint64_t> total_latency_us_field;
    rusty::Cell<uint64_t> min_latency_us_field;
    rusty::Cell<uint64_t> max_latency_us_field;

    ConnectionMetrics();
    uint64_t requests_sent() const;
    uint64_t requests_completed() const;
    uint64_t requests_failed() const;
    uint64_t requests_timed_out() const;
    uint64_t in_flight_requests() const;
    uint64_t bytes_sent() const;
    uint64_t bytes_received() const;
    uint64_t reconnect_count() const;
    uint64_t retry_attempts() const;
    uint64_t queue_dropped_requests() const;
    uint64_t circuit_open_rejections() const;
    uint64_t circuit_open_transitions() const;
    uint64_t circuit_half_open_transitions() const;
    uint64_t circuit_closed_transitions() const;
    uint64_t connect_time_ms() const;
    uint64_t min_latency_us() const;
    uint64_t max_latency_us() const;
    uint64_t success_rate_percent() const;
    uint64_t avg_latency_us() const;
    uint64_t uptime_ms(uint64_t current_time_ms) const;
    void record_request_sent() const;
    void record_request_completed_with_latency(uint64_t latency_us) const;
    void record_request_completed() const;
    void record_request_failed() const;
    void record_request_timeout() const;
    void record_request_dropped() const;
    void record_bytes_sent(uint64_t bytes) const;
    void record_bytes_received(uint64_t bytes) const;
    void record_reconnect() const;
    void record_retry_attempt() const;
    void record_queue_drop() const;
    void record_circuit_open_rejection() const;
    void record_circuit_open_transition() const;
    void record_circuit_half_open_transition() const;
    void record_circuit_closed_transition() const;
    void record_connect(uint64_t current_time_ms) const;
    void reset() const;
    void decrement_in_flight() const;
};


ConnectionMetrics::ConnectionMetrics()
    : requests_sent_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , requests_completed_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , requests_failed_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , requests_timed_out_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , in_flight_requests_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , bytes_sent_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , bytes_received_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , reconnect_count_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , retry_attempts_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , queue_dropped_requests_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , circuit_open_rejections_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , circuit_open_transitions_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , circuit_half_open_transitions_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , circuit_closed_transitions_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , connect_time_ms_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , total_latency_us_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , min_latency_us_field(rusty::Cell<uint64_t>::new_(std::numeric_limits<uint64_t>::max()))
    , max_latency_us_field(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
{}

uint64_t ConnectionMetrics::requests_sent() const {
    return this->requests_sent_field.get();
}

uint64_t ConnectionMetrics::requests_completed() const {
    return this->requests_completed_field.get();
}

uint64_t ConnectionMetrics::requests_failed() const {
    return this->requests_failed_field.get();
}

uint64_t ConnectionMetrics::requests_timed_out() const {
    return this->requests_timed_out_field.get();
}

uint64_t ConnectionMetrics::in_flight_requests() const {
    return this->in_flight_requests_field.get();
}

uint64_t ConnectionMetrics::bytes_sent() const {
    return this->bytes_sent_field.get();
}

uint64_t ConnectionMetrics::bytes_received() const {
    return this->bytes_received_field.get();
}

uint64_t ConnectionMetrics::reconnect_count() const {
    return this->reconnect_count_field.get();
}

uint64_t ConnectionMetrics::retry_attempts() const {
    return this->retry_attempts_field.get();
}

uint64_t ConnectionMetrics::queue_dropped_requests() const {
    return this->queue_dropped_requests_field.get();
}

uint64_t ConnectionMetrics::circuit_open_rejections() const {
    return this->circuit_open_rejections_field.get();
}

uint64_t ConnectionMetrics::circuit_open_transitions() const {
    return this->circuit_open_transitions_field.get();
}

uint64_t ConnectionMetrics::circuit_half_open_transitions() const {
    return this->circuit_half_open_transitions_field.get();
}

uint64_t ConnectionMetrics::circuit_closed_transitions() const {
    return this->circuit_closed_transitions_field.get();
}

uint64_t ConnectionMetrics::connect_time_ms() const {
    return this->connect_time_ms_field.get();
}

uint64_t ConnectionMetrics::min_latency_us() const {
    uint64_t min = this->min_latency_us_field.get();
    if (rusty::detail::deref_if_pointer_like(min) == rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max())) {
        return static_cast<uint64_t>(0);
    } else {
        return std::move(min);
    }
}

uint64_t ConnectionMetrics::max_latency_us() const {
    return this->max_latency_us_field.get();
}

uint64_t ConnectionMetrics::success_rate_percent() const {
    const uint64_t completed = this->requests_completed_field.get();
    const uint64_t total = this->requests_sent_field.get();
    if (rusty::detail::deref_if_pointer_like(total) == static_cast<uint64_t>(0)) {
        return static_cast<uint64_t>(100);
    }
    return ((rusty::detail::deref_if_pointer_like(completed) * static_cast<uint64_t>(100))) / rusty::detail::deref_if_pointer_like(total);
}

uint64_t ConnectionMetrics::avg_latency_us() const {
    const uint64_t completed = this->requests_completed_field.get();
    if (rusty::detail::deref_if_pointer_like(completed) == static_cast<uint64_t>(0)) {
        return static_cast<uint64_t>(0);
    }
    return this->total_latency_us_field.get() / rusty::detail::deref_if_pointer_like(completed);
}

uint64_t ConnectionMetrics::uptime_ms(uint64_t current_time_ms) const {
    const uint64_t connect_time = this->connect_time_ms_field.get();
    if (rusty::detail::deref_if_pointer_like(connect_time) == static_cast<uint64_t>(0)) {
        return static_cast<uint64_t>(0);
    }
    if (rusty::detail::deref_if_pointer_like(current_time_ms) < rusty::detail::deref_if_pointer_like(connect_time)) {
        return static_cast<uint64_t>(0);
    }
    return rusty::detail::deref_if_pointer_like(current_time_ms) - rusty::detail::deref_if_pointer_like(connect_time);
}

void ConnectionMetrics::record_request_sent() const {
    this->requests_sent_field.set(this->requests_sent_field.get() + static_cast<uint64_t>(1));
    this->in_flight_requests_field.set(this->in_flight_requests_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_request_completed_with_latency(uint64_t latency_us) const {
    this->requests_completed_field.set(this->requests_completed_field.get() + static_cast<uint64_t>(1));
    this->decrement_in_flight();
    this->total_latency_us_field.set(this->total_latency_us_field.get() + rusty::detail::deref_if_pointer_like(latency_us));
    const uint64_t current_min = this->min_latency_us_field.get();
    if (rusty::detail::deref_if_pointer_like(latency_us) < rusty::detail::deref_if_pointer_like(current_min)) {
        this->min_latency_us_field.set(std::move(latency_us));
    }
    const uint64_t current_max = this->max_latency_us_field.get();
    if (rusty::detail::deref_if_pointer_like(latency_us) > rusty::detail::deref_if_pointer_like(current_max)) {
        this->max_latency_us_field.set(std::move(latency_us));
    }
}

void ConnectionMetrics::record_request_completed() const {
    this->requests_completed_field.set(this->requests_completed_field.get() + static_cast<uint64_t>(1));
    this->decrement_in_flight();
}

void ConnectionMetrics::record_request_failed() const {
    this->requests_failed_field.set(this->requests_failed_field.get() + static_cast<uint64_t>(1));
    this->decrement_in_flight();
}

void ConnectionMetrics::record_request_timeout() const {
    this->requests_timed_out_field.set(this->requests_timed_out_field.get() + static_cast<uint64_t>(1));
    this->decrement_in_flight();
}

void ConnectionMetrics::record_request_dropped() const {
    this->decrement_in_flight();
}

void ConnectionMetrics::record_bytes_sent(uint64_t bytes) const {
    this->bytes_sent_field.set(this->bytes_sent_field.get() + rusty::detail::deref_if_pointer_like(bytes));
}

void ConnectionMetrics::record_bytes_received(uint64_t bytes) const {
    this->bytes_received_field.set(this->bytes_received_field.get() + rusty::detail::deref_if_pointer_like(bytes));
}

void ConnectionMetrics::record_reconnect() const {
    this->reconnect_count_field.set(this->reconnect_count_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_retry_attempt() const {
    this->retry_attempts_field.set(this->retry_attempts_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_queue_drop() const {
    this->queue_dropped_requests_field.set(this->queue_dropped_requests_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_circuit_open_rejection() const {
    this->circuit_open_rejections_field.set(this->circuit_open_rejections_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_circuit_open_transition() const {
    this->circuit_open_transitions_field.set(this->circuit_open_transitions_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_circuit_half_open_transition() const {
    this->circuit_half_open_transitions_field.set(this->circuit_half_open_transitions_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_circuit_closed_transition() const {
    this->circuit_closed_transitions_field.set(this->circuit_closed_transitions_field.get() + static_cast<uint64_t>(1));
}

void ConnectionMetrics::record_connect(uint64_t current_time_ms) const {
    this->connect_time_ms_field.set(std::move(current_time_ms));
}

void ConnectionMetrics::reset() const {
    this->requests_sent_field.set(static_cast<uint64_t>(0));
    this->requests_completed_field.set(static_cast<uint64_t>(0));
    this->requests_failed_field.set(static_cast<uint64_t>(0));
    this->requests_timed_out_field.set(static_cast<uint64_t>(0));
    this->in_flight_requests_field.set(static_cast<uint64_t>(0));
    this->bytes_sent_field.set(static_cast<uint64_t>(0));
    this->bytes_received_field.set(static_cast<uint64_t>(0));
    this->reconnect_count_field.set(static_cast<uint64_t>(0));
    this->retry_attempts_field.set(static_cast<uint64_t>(0));
    this->queue_dropped_requests_field.set(static_cast<uint64_t>(0));
    this->circuit_open_rejections_field.set(static_cast<uint64_t>(0));
    this->circuit_open_transitions_field.set(static_cast<uint64_t>(0));
    this->circuit_half_open_transitions_field.set(static_cast<uint64_t>(0));
    this->circuit_closed_transitions_field.set(static_cast<uint64_t>(0));
    this->connect_time_ms_field.set(static_cast<uint64_t>(0));
    this->total_latency_us_field.set(static_cast<uint64_t>(0));
    this->min_latency_us_field.set(std::numeric_limits<uint64_t>::max());
    this->max_latency_us_field.set(static_cast<uint64_t>(0));
}

void ConnectionMetrics::decrement_in_flight() const {
    const uint64_t in_flight = this->in_flight_requests_field.get();
    if (rusty::detail::deref_if_pointer_like(in_flight) == static_cast<uint64_t>(0)) {
        return;
    }
    this->in_flight_requests_field.set(rusty::detail::deref_if_pointer_like(in_flight) - static_cast<uint64_t>(1));
}
/*RUSTYCPP:GEN-END id=connection_metrics.1*/

}  // export namespace rrr
