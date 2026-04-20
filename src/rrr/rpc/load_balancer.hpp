module;
/**
 * Load balancing strategies for RPC client pools.
 * Part of Phase 5.2: Load Balancing Strategies.
 *
 * Provides different strategies for selecting clients from a pool:
 * - RANDOM: Random selection (default, fast)
 * - ROUND_ROBIN: Sequential cycling through clients
 * - LEAST_CONNECTIONS: Select client with fewest in-flight requests
 * - LEAST_LATENCY: Select client with lowest average latency
 */


#include <cstdint>
#include <vector>
#include <rusty/cell.hpp>
#include <rusty/arc.hpp>

export module rrr:rpc.load_balancer;

import :rpc.connection_metrics;

export namespace rrr {

// Forward declaration
class Client;

/**
 * @safe - Enum representing load balancing strategies.
 */
enum class LoadBalancingStrategy : uint8_t {
    RANDOM = 0,           // Random selection (default)
    ROUND_ROBIN = 1,      // Sequential cycling
    LEAST_CONNECTIONS = 2, // Fewest in-flight requests
    LEAST_LATENCY = 3     // Lowest average latency
};

// @safe - Convert strategy to string for logging
inline const char* load_balancing_strategy_to_string(LoadBalancingStrategy strategy) {
    switch (strategy) {
        case LoadBalancingStrategy::RANDOM: return "RANDOM";
        case LoadBalancingStrategy::ROUND_ROBIN: return "ROUND_ROBIN";
        case LoadBalancingStrategy::LEAST_CONNECTIONS: return "LEAST_CONNECTIONS";
        case LoadBalancingStrategy::LEAST_LATENCY: return "LEAST_LATENCY";
        default: return "UNKNOWN";
    }
}

/**
 * @safe - Load balancer state for tracking selection state.
 * Uses rusty::Cell for thread-safe interior mutability.
 */
class LoadBalancerState {
    // Round-robin index per address
    rusty::Cell<size_t> round_robin_index_{0};

public:
    // @safe - Get next index for round-robin
    size_t next_round_robin_index(size_t pool_size) {
        if (pool_size == 0) return 0;
        size_t current = round_robin_index_.get();
        size_t next = (current + 1) % pool_size;
        round_robin_index_.set(next);
        return current;
    }

    // @safe - Reset state
    void reset() {
        round_robin_index_.set(0);
    }
};

/**
 * @safe - Helper class for load balancing selection logic.
 * Stateless - all state is in LoadBalancerState.
 */
class LoadBalancer {
public:
    /**
     * @safe - Select a client index using the specified strategy.
     *
     * @param strategy The load balancing strategy to use
     * @param clients Vector of clients to choose from
     * @param state State for stateful strategies (e.g., round-robin)
     * @param rand_value Random value for RANDOM strategy
     * @return Selected index, or pool_size if no suitable client found
     */
    template<typename ClientVec>
    static size_t select(
        LoadBalancingStrategy strategy,
        const ClientVec& clients,
        LoadBalancerState& state,
        size_t rand_value
    ) {
        size_t pool_size = clients.size();
        if (pool_size == 0) return 0;

        switch (strategy) {
            case LoadBalancingStrategy::RANDOM:
                return select_random(pool_size, rand_value);

            case LoadBalancingStrategy::ROUND_ROBIN:
                return select_round_robin(pool_size, state);

            case LoadBalancingStrategy::LEAST_CONNECTIONS:
                return select_least_connections(clients);

            case LoadBalancingStrategy::LEAST_LATENCY:
                return select_least_latency(clients);

            default:
                return select_random(pool_size, rand_value);
        }
    }

private:
    // @safe - Random selection
    static size_t select_random(size_t pool_size, size_t rand_value) {
        return rand_value % pool_size;
    }

    // @safe - Round-robin selection
    static size_t select_round_robin(size_t pool_size, LoadBalancerState& state) {
        return state.next_round_robin_index(pool_size);
    }

    // @safe - Select client with smallest explicit in-flight request count.
    template<typename ClientVec>
    static size_t select_least_connections(const ClientVec& clients) {
        size_t best_idx = 0;
        uint64_t min_pending = UINT64_MAX;

        for (size_t i = 0; i < clients.size(); i++) {
            const auto& client = clients[i];
            const auto& metrics = client->metrics();
            uint64_t pending = metrics.in_flight_requests();
            if (pending < min_pending) {
                min_pending = pending;
                best_idx = i;
            }
        }

        return best_idx;
    }

    // @safe - Select client with lowest average latency
    template<typename ClientVec>
    static size_t select_least_latency(const ClientVec& clients) {
        size_t best_idx = 0;
        uint64_t min_latency = UINT64_MAX;

        for (size_t i = 0; i < clients.size(); i++) {
            const auto& client = clients[i];
            const auto& metrics = client->metrics();
            uint64_t avg_latency = metrics.avg_latency_us();

            // Skip clients with no latency data (use max latency)
            if (avg_latency == 0 && metrics.requests_completed() == 0) {
                continue;
            }

            if (avg_latency < min_latency) {
                min_latency = avg_latency;
                best_idx = i;
            }
        }

        return best_idx;
    }
};

} // namespace rrr
