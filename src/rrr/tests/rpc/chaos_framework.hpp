/**
 * Chaos Engineering Test Framework
 * Chaos Engineering Tests
 *
 * Provides infrastructure for injecting controlled failures and
 * verifying system recovery in RPC reliability tests.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <thread>
#include <vector>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>

namespace rrr {
namespace chaos {

// ============================================================================
// FailureType - Types of chaos failures that can be injected
// ============================================================================

// @safe - Failure types enum
enum class FailureType : uint8_t {
    NONE = 0,
    SERVER_KILL = 1,        // Kill and restart server
    LATENCY_INJECTION = 2,  // Add artificial delay
    CONNECTION_RESET = 3,   // Force disconnect clients
    PACKET_LOSS = 4,        // Simulate lost messages (timeout)
    COMBINED = 5            // Random combination
};

// @safe - Convert failure type to string
inline const char* failure_type_to_string(FailureType type) {
    switch (type) {
        case FailureType::NONE: return "NONE";
        case FailureType::SERVER_KILL: return "SERVER_KILL";
        case FailureType::LATENCY_INJECTION: return "LATENCY_INJECTION";
        case FailureType::CONNECTION_RESET: return "CONNECTION_RESET";
        case FailureType::PACKET_LOSS: return "PACKET_LOSS";
        case FailureType::COMBINED: return "COMBINED";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// ChaosConfig - Configuration for chaos injection
// ============================================================================

// @safe - Configuration for chaos tests
struct ChaosConfig {
    double failure_rate = 0.1;           // Probability of failure per check (0.0-1.0)
    uint32_t check_interval_ms = 100;    // How often to potentially inject failure
    uint32_t duration_ms = 5000;         // Total chaos duration
    uint32_t recovery_timeout_ms = 2000; // Time to wait for recovery
    uint32_t latency_min_ms = 10;        // Min latency for LATENCY_INJECTION
    uint32_t latency_max_ms = 500;       // Max latency for LATENCY_INJECTION
    uint32_t server_restart_delay_ms = 100; // Delay before restarting killed server
    bool auto_restart_server = true;     // Auto restart server after kill
    uint32_t seed = 0;                   // RNG seed (0 = use random seed)

    // @safe - Default configuration
    static ChaosConfig defaults() {
        return ChaosConfig{};
    }

    // @safe - Aggressive chaos configuration
    static ChaosConfig aggressive() {
        ChaosConfig cfg;
        cfg.failure_rate = 0.3;
        cfg.check_interval_ms = 50;
        cfg.duration_ms = 10000;
        return cfg;
    }

    // @safe - Light chaos configuration
    static ChaosConfig light() {
        ChaosConfig cfg;
        cfg.failure_rate = 0.05;
        cfg.check_interval_ms = 200;
        cfg.duration_ms = 3000;
        return cfg;
    }
};

// ============================================================================
// ChaosStatsSnapshot - Copyable snapshot of chaos statistics
// ============================================================================

// @safe - Copyable snapshot of statistics
struct ChaosStatsSnapshot {
    uint64_t server_kills = 0;
    uint64_t latency_injections = 0;
    uint64_t connection_resets = 0;
    uint64_t packet_losses = 0;
    uint64_t total_failures = 0;
    uint64_t requests_during_chaos = 0;
    uint64_t requests_succeeded = 0;
    uint64_t requests_failed = 0;
};

// ============================================================================
// ChaosStats - Statistics collected during chaos
// ============================================================================

// @safe - Thread-safe statistics
struct ChaosStats {
    std::atomic<uint64_t> server_kills{0};
    std::atomic<uint64_t> latency_injections{0};
    std::atomic<uint64_t> connection_resets{0};
    std::atomic<uint64_t> packet_losses{0};
    std::atomic<uint64_t> total_failures{0};
    std::atomic<uint64_t> requests_during_chaos{0};
    std::atomic<uint64_t> requests_succeeded{0};
    std::atomic<uint64_t> requests_failed{0};

    // @safe - Reset all stats
    void reset() {
        server_kills = 0;
        latency_injections = 0;
        connection_resets = 0;
        packet_losses = 0;
        total_failures = 0;
        requests_during_chaos = 0;
        requests_succeeded = 0;
        requests_failed = 0;
    }

    // @safe - Get failure count for specific type
    uint64_t get_count(FailureType type) const {
        switch (type) {
            case FailureType::SERVER_KILL: return server_kills.load();
            case FailureType::LATENCY_INJECTION: return latency_injections.load();
            case FailureType::CONNECTION_RESET: return connection_resets.load();
            case FailureType::PACKET_LOSS: return packet_losses.load();
            default: return 0;
        }
    }

    // @safe - Increment count for specific type
    void increment(FailureType type) {
        total_failures++;
        switch (type) {
            case FailureType::SERVER_KILL: server_kills++; break;
            case FailureType::LATENCY_INJECTION: latency_injections++; break;
            case FailureType::CONNECTION_RESET: connection_resets++; break;
            case FailureType::PACKET_LOSS: packet_losses++; break;
            default: break;
        }
    }

    // @safe - Create a copyable snapshot
    ChaosStatsSnapshot snapshot() const {
        ChaosStatsSnapshot snap;
        snap.server_kills = server_kills.load();
        snap.latency_injections = latency_injections.load();
        snap.connection_resets = connection_resets.load();
        snap.packet_losses = packet_losses.load();
        snap.total_failures = total_failures.load();
        snap.requests_during_chaos = requests_during_chaos.load();
        snap.requests_succeeded = requests_succeeded.load();
        snap.requests_failed = requests_failed.load();
        return snap;
    }
};

// ============================================================================
// ChaosResult - Result of a chaos test run
// ============================================================================

// @safe - Result of chaos test
struct ChaosResult {
    ChaosStatsSnapshot stats;
    uint64_t recovery_time_ms = 0;
    bool connectivity_verified = false;
    bool requests_verified = false;
    bool passed = false;

    // @safe - Check if all verifications passed
    bool all_verified() const {
        return connectivity_verified && requests_verified;
    }
};

// ============================================================================
// ChaosController - Controls chaos injection
// ============================================================================

// @unsafe - Thread-safe chaos controller
class ChaosController {
    ChaosConfig config_;
    ChaosStats stats_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    rusty::Cell<FailureType> active_failure_type_{FailureType::NONE};
    rusty::Cell<uint32_t> current_latency_ms_{0};

    // RNG for random failures
    std::mt19937 rng_;
    std::uniform_real_distribution<double> failure_dist_{0.0, 1.0};
    std::uniform_int_distribution<uint32_t> latency_dist_;
    std::uniform_int_distribution<int> type_dist_{1, 4};

public:
    // @safe - Callbacks for chaos events
    using ServerKillCallback = rusty::Function<void()>;
    using ServerRestartCallback = rusty::Function<void()>;
    using ConnectionResetCallback = rusty::Function<void()>;

private:
    ServerKillCallback on_server_kill_;
    ServerRestartCallback on_server_restart_;
    ConnectionResetCallback on_connection_reset_;

public:
    // @safe - Constructor
    explicit ChaosController(const ChaosConfig& config = ChaosConfig::defaults())
        : config_(config)
    {
        // Initialize RNG
        uint32_t seed = config.seed;
        if (seed == 0) {
            std::random_device rd;
            seed = rd();
        }
        rng_.seed(seed);
        latency_dist_ = std::uniform_int_distribution<uint32_t>(
            config.latency_min_ms, config.latency_max_ms
        );
    }

    // @safe - Set server kill callback
    void set_on_server_kill(ServerKillCallback cb) {
        on_server_kill_ = std::move(cb);
    }

    // @safe - Set server restart callback
    void set_on_server_restart(ServerRestartCallback cb) {
        on_server_restart_ = std::move(cb);
    }

    // @safe - Set connection reset callback
    void set_on_connection_reset(ConnectionResetCallback cb) {
        on_connection_reset_ = std::move(cb);
    }

    // @safe - Check if chaos is running
    bool is_running() const {
        return running_.load();
    }

    // @safe - Check if chaos is paused
    bool is_paused() const {
        return paused_.load();
    }

    // @safe - Start chaos (non-blocking)
    void start() {
        running_ = true;
        paused_ = false;
    }

    // @safe - Stop chaos
    void stop() {
        running_ = false;
        paused_ = false;
    }

    // @safe - Pause chaos without stopping
    void pause() {
        paused_ = true;
    }

    // @safe - Resume paused chaos
    void resume() {
        paused_ = false;
    }

    // @safe - Get current stats
    const ChaosStats& stats() const {
        return stats_;
    }

    // @safe - Reset stats
    void reset_stats() {
        stats_.reset();
    }

    // @safe - Get config
    const ChaosConfig& config() const {
        return config_;
    }

    // @safe - Get current latency (for latency injection)
    uint32_t current_latency_ms() const {
        return current_latency_ms_.get();
    }

    /**
     * @unsafe - Check and potentially inject failure
     *
     * Call this periodically from test loop. Returns the type of failure
     * that was injected (or NONE if no failure was injected).
     */
    FailureType maybe_inject_failure(FailureType allowed_type = FailureType::COMBINED) {
        if (!running_ || paused_) {
            return FailureType::NONE;
        }

        // Check if we should inject failure
        double roll = failure_dist_(rng_);
        if (roll >= config_.failure_rate) {
            return FailureType::NONE;
        }

        // Determine failure type
        FailureType type_to_inject = allowed_type;
        if (allowed_type == FailureType::COMBINED) {
            // Pick random type
            int type_int = type_dist_(rng_);
            type_to_inject = static_cast<FailureType>(type_int);
        }

        return inject_failure(type_to_inject);
    }

    /**
     * @unsafe - Manually inject a specific failure
     */
    FailureType inject_failure(FailureType type) {
        if (type == FailureType::NONE) {
            return FailureType::NONE;
        }

        active_failure_type_.set(type);
        stats_.increment(type);

        switch (type) {
            case FailureType::SERVER_KILL:
                if (on_server_kill_) {
                    on_server_kill_();
                }
                // Auto restart after delay if configured
                if (config_.auto_restart_server && on_server_restart_) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(config_.server_restart_delay_ms)
                    );
                    on_server_restart_();
                }
                break;

            case FailureType::LATENCY_INJECTION:
                current_latency_ms_.set(latency_dist_(rng_));
                break;

            case FailureType::CONNECTION_RESET:
                if (on_connection_reset_) {
                    on_connection_reset_();
                }
                break;

            case FailureType::PACKET_LOSS:
                // Packet loss is simulated by caller (timeout handling)
                break;

            default:
                break;
        }

        return type;
    }

    // @safe - Clear active failure (for latency injection)
    void clear_latency() {
        current_latency_ms_.set(0);
        if (active_failure_type_.get() == FailureType::LATENCY_INJECTION) {
            active_failure_type_.set(FailureType::NONE);
        }
    }
};

// ============================================================================
// ChaosVerifier - Verifies system recovery after chaos
// ============================================================================

// @safe - Verifier for chaos recovery
class ChaosVerifier {
    uint32_t timeout_ms_;
    rusty::Cell<uint64_t> recovery_start_ms_{0};
    rusty::Cell<uint64_t> recovery_end_ms_{0};

public:
    // @safe - Callbacks for verification
    using ConnectivityCheck = rusty::Function<bool()>;
    using RequestCheck = rusty::Function<bool()>;

private:
    ConnectivityCheck connectivity_check_;
    RequestCheck request_check_;

public:
    // @safe - Constructor
    explicit ChaosVerifier(uint32_t timeout_ms = 2000)
        : timeout_ms_(timeout_ms) {}

    // @safe - Set connectivity check callback
    void set_connectivity_check(ConnectivityCheck cb) {
        connectivity_check_ = std::move(cb);
    }

    // @safe - Set request check callback
    void set_request_check(RequestCheck cb) {
        request_check_ = std::move(cb);
    }

    /**
     * @unsafe - Verify connectivity with retries
     *
     * Returns true if connectivity is established within timeout.
     */
    bool verify_connectivity() {
        if (!connectivity_check_) {
            return true;  // No check defined, assume OK
        }

        auto start = std::chrono::steady_clock::now();
        recovery_start_ms_.set(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                start.time_since_epoch()
            ).count()
        );

        while (true) {
            if (connectivity_check_()) {
                auto end = std::chrono::steady_clock::now();
                recovery_end_ms_.set(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        end.time_since_epoch()
                    ).count()
                );
                return true;
            }

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                >= timeout_ms_) {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    /**
     * @unsafe - Verify requests complete successfully
     *
     * Returns true if requests succeed within timeout.
     */
    bool verify_requests() {
        if (!request_check_) {
            return true;  // No check defined, assume OK
        }

        auto start = std::chrono::steady_clock::now();

        while (true) {
            if (request_check_()) {
                return true;
            }

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                >= timeout_ms_) {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    /**
     * @unsafe - Run full verification and return result
     */
    ChaosResult run_verification(ChaosController& controller) {
        ChaosResult result;
        result.stats = controller.stats().snapshot();

        result.connectivity_verified = verify_connectivity();
        if (result.connectivity_verified) {
            result.requests_verified = verify_requests();
        }

        result.recovery_time_ms = recovery_end_ms_.get() - recovery_start_ms_.get();
        result.passed = result.all_verified();

        return result;
    }

    // @safe - Get recovery time in ms
    uint64_t get_recovery_time_ms() const {
        return recovery_end_ms_.get() - recovery_start_ms_.get();
    }
};

// ============================================================================
// ChaosScenario - Pre-defined chaos scenarios
// ============================================================================

// @safe - Pre-defined chaos scenario
struct ChaosScenario {
    const char* name;
    FailureType failure_type;
    ChaosConfig config;

    // @safe - Random server kills scenario
    static ChaosScenario random_server_kills() {
        ChaosScenario scenario;
        scenario.name = "RandomServerKills";
        scenario.failure_type = FailureType::SERVER_KILL;
        scenario.config = ChaosConfig::defaults();
        scenario.config.failure_rate = 0.2;
        scenario.config.check_interval_ms = 200;
        return scenario;
    }

    // @safe - Latency spikes scenario
    static ChaosScenario latency_spikes() {
        ChaosScenario scenario;
        scenario.name = "LatencySpikes";
        scenario.failure_type = FailureType::LATENCY_INJECTION;
        scenario.config = ChaosConfig::defaults();
        scenario.config.failure_rate = 0.3;
        scenario.config.latency_min_ms = 100;
        scenario.config.latency_max_ms = 1000;
        return scenario;
    }

    // @safe - Connection churn scenario
    static ChaosScenario connection_churn() {
        ChaosScenario scenario;
        scenario.name = "ConnectionChurn";
        scenario.failure_type = FailureType::CONNECTION_RESET;
        scenario.config = ChaosConfig::defaults();
        scenario.config.failure_rate = 0.25;
        scenario.config.check_interval_ms = 100;
        return scenario;
    }

    // @safe - Combined chaos scenario
    static ChaosScenario combined_chaos() {
        ChaosScenario scenario;
        scenario.name = "CombinedChaos";
        scenario.failure_type = FailureType::COMBINED;
        scenario.config = ChaosConfig::aggressive();
        return scenario;
    }
};

} // namespace chaos
} // namespace rrr
