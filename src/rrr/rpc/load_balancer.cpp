module;

#include <rusty/cell.hpp>
#include <rusty/arc.hpp>
#include <cstdint>

// Forward decl in GMF — Client is fully declared in client.hpp (global
// module) and is only referenced here as a name. Declaring it in
// module purview would give the forward-decl module attachment that
// clashes with client.hpp's global-module declaration.
namespace rrr { class Client; }

export module rrr.load_balancer;

import std;

export namespace rrr {

enum class LoadBalancingStrategy : uint8_t {
    RANDOM = 0,
    ROUND_ROBIN = 1,
    LEAST_CONNECTIONS = 2,
    LEAST_LATENCY = 3
};

inline const char* load_balancing_strategy_to_string(LoadBalancingStrategy strategy) {
    switch (strategy) {
        case LoadBalancingStrategy::RANDOM: return "RANDOM";
        case LoadBalancingStrategy::ROUND_ROBIN: return "ROUND_ROBIN";
        case LoadBalancingStrategy::LEAST_CONNECTIONS: return "LEAST_CONNECTIONS";
        case LoadBalancingStrategy::LEAST_LATENCY: return "LEAST_LATENCY";
        default: return "UNKNOWN";
    }
}

class LoadBalancerState {
    rusty::Cell<size_t> round_robin_index_{0};

public:
    size_t next_round_robin_index(size_t pool_size) {
        if (pool_size == 0) return 0;
        size_t current = round_robin_index_.get();
        size_t next = (current + 1) % pool_size;
        round_robin_index_.set(next);
        return current;
    }

    void reset() {
        round_robin_index_.set(0);
    }
};

class LoadBalancer {
public:
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
    static size_t select_random(size_t pool_size, size_t rand_value) {
        return rand_value % pool_size;
    }

    static size_t select_round_robin(size_t pool_size, LoadBalancerState& state) {
        return state.next_round_robin_index(pool_size);
    }

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

    template<typename ClientVec>
    static size_t select_least_latency(const ClientVec& clients) {
        size_t best_idx = 0;
        uint64_t min_latency = UINT64_MAX;

        for (size_t i = 0; i < clients.size(); i++) {
            const auto& client = clients[i];
            const auto& metrics = client->metrics();
            uint64_t avg_latency = metrics.avg_latency_us();

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

} // export namespace rrr
