module;

#include <rusty/cell.hpp>

export module rrr:rpc.heartbeat;

import <functional>;
import <cstdint>;
import <ctime>;

export namespace rrr {

// @safe - Get current time in microseconds
inline uint64_t heartbeat_time_us() {
    // @unsafe - system call
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    }
}

/**
 * Configuration for heartbeat mechanism.
 */
struct HeartbeatConfig {
    bool enabled;              // Enable/disable heartbeat (default: true)
    uint32_t interval_ms;      // Ping interval in milliseconds (default: 10000 = 10s)
    uint32_t timeout_ms;       // Pong timeout in milliseconds (default: 5000 = 5s)
    uint32_t max_missed;       // Max missed pongs before timeout (default: 3)

    // @safe - Default constructor with reasonable defaults
    HeartbeatConfig()
        : enabled(true)
        , interval_ms(10000)   // 10 seconds
        , timeout_ms(5000)     // 5 seconds
        , max_missed(3)
    {}

    // @safe - Full constructor
    HeartbeatConfig(
        bool enabled_,
        uint32_t interval_ms_,
        uint32_t timeout_ms_,
        uint32_t max_missed_
    )
        : enabled(enabled_)
        , interval_ms(interval_ms_)
        , timeout_ms(timeout_ms_)
        , max_missed(max_missed_)
    {}

    // =========================================================================
    // Config Presets
    // =========================================================================

    // @safe - Aggressive heartbeat: frequent pings, quick timeout
    static HeartbeatConfig aggressive() {
        return HeartbeatConfig(
            true,     // enabled
            5000,     // interval_ms (5 seconds)
            2000,     // timeout_ms (2 seconds)
            2         // max_missed
        );
    }

    // @safe - Relaxed heartbeat: infrequent pings, patient timeout
    static HeartbeatConfig relaxed() {
        return HeartbeatConfig(
            true,     // enabled
            30000,    // interval_ms (30 seconds)
            15000,    // timeout_ms (15 seconds)
            5         // max_missed
        );
    }

    // @safe - Disabled heartbeat
    static HeartbeatConfig disabled() {
        return HeartbeatConfig(
            false,    // enabled
            0,        // interval_ms
            0,        // timeout_ms
            0         // max_missed
        );
    }
};

/**
 * Heartbeat manager for detecting stale connections.
 *
 * This class tracks heartbeat state but does NOT manage timers.
 * The caller is responsible for:
 * 1. Calling should_send_heartbeat() periodically to check if a heartbeat should be sent
 * 2. Sending the actual heartbeat (e.g., RPC call)
 * 3. Calling on_heartbeat_sent() after sending
 * 4. Calling on_pong_received() when response is received
 * 5. Calling check_timeout() periodically to detect failures
 *
 * Usage:
 *   HeartbeatManager hb(config);
 *   hb.set_on_timeout([&]() { client.reconnect(); });
 *
 *   // In periodic poll:
 *   if (hb.should_send_heartbeat()) {
 *       client.send_ping();
 *       hb.on_heartbeat_sent();
 *   }
 *   if (hb.check_timeout()) {
 *       // on_timeout callback was invoked
 *   }
 *
 *   // When pong received:
 *   hb.on_pong_received();
 */
// @safe - Thread-safe heartbeat state management
class HeartbeatManager {
private:
    HeartbeatConfig config_;

    // Timestamps in microseconds
    rusty::Cell<uint64_t> last_send_time_{0};
    rusty::Cell<uint64_t> last_recv_time_{0};

    // State tracking
    rusty::Cell<uint32_t> missed_count_{0};
    rusty::Cell<bool> pending_pong_{false};
    rusty::Cell<bool> timed_out_{false};

    // Callback for timeout
    std::function<void()> on_timeout_;

public:
    // @safe - Constructor with config
    explicit HeartbeatManager(const HeartbeatConfig& config = HeartbeatConfig())
        : config_(config)
    {}

    // @safe - Replace runtime config and reset heartbeat state.
    void set_config(const HeartbeatConfig& config) {
        // @unsafe - assignment operator is currently modeled as non-safe.
        { config_ = config; }
        reset();
    }

    // @safe - Set timeout callback
    void set_on_timeout(std::function<void()> callback) {
        on_timeout_ = std::move(callback);
    }

    // @safe - Check if it's time to send a heartbeat
    bool should_send_heartbeat() const {
        if (!config_.enabled || timed_out_.get()) {
            return false;
        }

        // Don't send if there's a pending pong
        if (pending_pong_.get()) {
            return false;
        }

        uint64_t now = heartbeat_time_us();
        uint64_t last = last_send_time_.get();
        uint64_t interval_us = static_cast<uint64_t>(config_.interval_ms) * 1000;

        return (now - last >= interval_us);
    }

    // @safe - Record that a heartbeat was sent
    void on_heartbeat_sent() {
        if (!config_.enabled) return;

        last_send_time_.set(heartbeat_time_us());
        pending_pong_.set(true);
    }

    // @safe - Record that a pong was received
    void on_pong_received() {
        if (!config_.enabled) return;

        last_recv_time_.set(heartbeat_time_us());
        pending_pong_.set(false);
        missed_count_.set(0);
        timed_out_.set(false);  // Reset timeout on successful pong
    }

    // @safe - Check if heartbeat has timed out
    // Returns true if timeout just occurred (triggers callback)
    bool check_timeout() {
        if (!config_.enabled || timed_out_.get()) {
            return false;
        }

        // Only check if there's a pending pong
        if (!pending_pong_.get()) {
            return false;
        }

        uint64_t now = heartbeat_time_us();
        uint64_t sent = last_send_time_.get();
        uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

        if (now - sent >= timeout_us) {
            // Pong timed out
            pending_pong_.set(false);
            uint32_t count = missed_count_.get() + 1;
            missed_count_.set(count);

            if (count >= config_.max_missed) {
                // Too many missed pongs, trigger timeout
                timed_out_.set(true);

                // Invoke callback
                // @unsafe - std::function::operator bool
                {
                    if (on_timeout_) {
                        on_timeout_();
                    }
                }
                return true;
            }
        }

        return false;
    }

    // @safe - Get time until next heartbeat should be sent (in ms)
    // Returns 0 if heartbeat should be sent now
    uint32_t time_until_next_heartbeat_ms() const {
        if (!config_.enabled || timed_out_.get() || pending_pong_.get()) {
            return config_.interval_ms;  // Return interval as default
        }

        uint64_t now = heartbeat_time_us();
        uint64_t last = last_send_time_.get();
        uint64_t interval_us = static_cast<uint64_t>(config_.interval_ms) * 1000;

        if (now - last >= interval_us) {
            return 0;
        }

        return static_cast<uint32_t>((interval_us - (now - last)) / 1000);
    }

    // @safe - Check if heartbeat has timed out (connection considered dead)
    bool is_timed_out() const {
        return timed_out_.get();
    }

    // @safe - Get number of missed heartbeats
    uint32_t missed_count() const {
        return missed_count_.get();
    }

    // @safe - Check if there's a pending pong
    bool is_pending_pong() const {
        return pending_pong_.get();
    }

    // @safe - Reset heartbeat state (e.g., after reconnection)
    void reset() {
        last_send_time_.set(0);
        last_recv_time_.set(0);
        missed_count_.set(0);
        pending_pong_.set(false);
        timed_out_.set(false);
    }

    // @safe - Get the configuration
    // @lifetime: (&'a) -> &'a
    const HeartbeatConfig& config() const {
        return config_;
    }
};

} // namespace rrr
