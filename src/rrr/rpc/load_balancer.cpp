module;

#include <rusty/cell.hpp>
#include <rusty/arc.hpp>
#include <rusty/move.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>
#include <cstdint>

export module rrr.load_balancer;

import std;

export namespace rrr {

// `LoadBalancingStrategy` — selection algorithm picked by the
// LoadBalancer. Authored as inline Rust DSL: the `#if RUSTYCPP_RUST`
// block below is the source of truth; the transpiler regenerates the
// matching `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
#[repr(u8)]
enum LoadBalancingStrategy {
    RANDOM = 0,
    ROUND_ROBIN = 1,
    LEAST_CONNECTIONS = 2,
    LEAST_LATENCY = 3,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=load_balancer.strategy version=1 rust_sha256=11bb5040a9e54302f28bb33abcd98c64893bcafd45c7d348ad0f77d281b71d5c*/
enum class LoadBalancingStrategy;
constexpr LoadBalancingStrategy LoadBalancingStrategy_RANDOM();
constexpr LoadBalancingStrategy LoadBalancingStrategy_ROUND_ROBIN();
constexpr LoadBalancingStrategy LoadBalancingStrategy_LEAST_CONNECTIONS();
constexpr LoadBalancingStrategy LoadBalancingStrategy_LEAST_LATENCY();

enum class LoadBalancingStrategy {
    RANDOM = 0,
    ROUND_ROBIN = 1,
    LEAST_CONNECTIONS = 2,
    LEAST_LATENCY = 3
};
inline constexpr LoadBalancingStrategy LoadBalancingStrategy_RANDOM() { return LoadBalancingStrategy::RANDOM; }
inline constexpr LoadBalancingStrategy LoadBalancingStrategy_ROUND_ROBIN() { return LoadBalancingStrategy::ROUND_ROBIN; }
inline constexpr LoadBalancingStrategy LoadBalancingStrategy_LEAST_CONNECTIONS() { return LoadBalancingStrategy::LEAST_CONNECTIONS; }
inline constexpr LoadBalancingStrategy LoadBalancingStrategy_LEAST_LATENCY() { return LoadBalancingStrategy::LEAST_LATENCY; }
/*RUSTYCPP:GEN-END id=load_balancer.strategy*/

inline const char* load_balancing_strategy_to_string(LoadBalancingStrategy strategy) {
    switch (strategy) {
        case LoadBalancingStrategy::RANDOM: return "RANDOM";
        case LoadBalancingStrategy::ROUND_ROBIN: return "ROUND_ROBIN";
        case LoadBalancingStrategy::LEAST_CONNECTIONS: return "LEAST_CONNECTIONS";
        case LoadBalancingStrategy::LEAST_LATENCY: return "LEAST_LATENCY";
        default: return "UNKNOWN";
    }
}

// `LoadBalancerState` — single-counter round-robin index holder.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new()` lowers
// to a `static LoadBalancerState new_()` factory; callers construct
// via the factory rather than direct ctor syntax.
//
// Behavioral diffs from the original C++ class:
//   * `next_round_robin_index()` becomes `const`. It only mutates the
//     `round_robin_index_field` Cell, which is interior-mutable.
//     Callers that held a non-const ref keep working.
//   * `reset()` becomes `const` for the same reason.
//   * The field is no longer marked `private`; no callers reach into
//     it. The trailing `_` is replaced with `_field` to dodge the
//     transpiler's field-vs-method collision detector (there is no
//     `round_robin_index()` accessor today, but the suffix makes the
//     intent explicit and matches the convention used by the other
//     DSL-migrated classes in this branch).
#if RUSTYCPP_RUST
struct LoadBalancerState {
    round_robin_index_field: Cell<usize>,
}

impl LoadBalancerState {
    fn new() -> LoadBalancerState {
        LoadBalancerState {
            round_robin_index_field: Cell::<usize>::new(0usize),
        }
    }

    fn next_round_robin_index(&self, pool_size: usize) -> usize {
        if pool_size == 0usize {
            return 0usize;
        }
        let current: usize = self.round_robin_index_field.get();
        let next: usize = (current + 1usize) % pool_size;
        self.round_robin_index_field.set(next);
        current
    }

    fn reset(&self) {
        self.round_robin_index_field.set(0usize);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=load_balancer.1 version=1 rust_sha256=9a65aa8a0df8c08d110e5f08193b07481fb250775b17a92d1a966f06e05fe705*/
struct LoadBalancerState;

struct LoadBalancerState {
    rusty::Cell<size_t> round_robin_index_field;

    static LoadBalancerState new_();
    size_t next_round_robin_index(size_t pool_size) const;
    void reset() const;
};


LoadBalancerState LoadBalancerState::new_() {
    return LoadBalancerState{.round_robin_index_field = rusty::Cell<size_t>::new_(static_cast<size_t>(0))};
}

size_t LoadBalancerState::next_round_robin_index(size_t pool_size) const {
    if (rusty::detail::deref_if_pointer_like(pool_size) == static_cast<size_t>(0)) {
        return static_cast<size_t>(0);
    }
    size_t current = this->round_robin_index_field.get();
    size_t next = ((rusty::detail::deref_if_pointer_like(current) + static_cast<size_t>(1))) % rusty::detail::deref_if_pointer_like(pool_size);
    this->round_robin_index_field.set(std::move(next));
    return std::move(current);
}

void LoadBalancerState::reset() const {
    this->round_robin_index_field.set(static_cast<size_t>(0));
}
/*RUSTYCPP:GEN-END id=load_balancer.1*/

// Hand-written generic helpers backing the DSL LoadBalancer below.
// They stay template free fns because they are generic over the client
// container (production: rusty::Vec<Arc<ClientConnection>>-shaped pools;
// tests: std::vector of mock clients) and their bodies arrow-deref the
// client handle — neither is DSL-expressible.

// @safe - container size via the generic `.size()`.
template<typename ClientVec>
size_t lb_pool_size(const ClientVec& clients) {
    return clients.size();
}

// @unsafe - generic arrow-deref of the client handle (Arc in production,
// raw/mock pointer in tests) to read its ConnectionMetrics.
template<typename ClientVec>
size_t lb_select_least_connections(const ClientVec& clients) {
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

// @unsafe - generic arrow-deref of the client handle (Arc in production,
// raw/mock pointer in tests) to read its ConnectionMetrics.
template<typename ClientVec>
size_t lb_select_least_latency(const ClientVec& clients) {
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

// `LoadBalancer` — pure stateless dispatch over LoadBalancingStrategy.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * The strategy dispatch is an if-else chain, not a `match`/`switch`
//     (a DSL `match` over a C-like enum lowers to broken variant_holds
//     C++; RANDOM and unknown strategies share the fall-through arm
//     exactly like the original `case RANDOM` + `default`).
//   * `select` takes `state` by const ref (was non-const ref); the only
//     state method it calls (`next_round_robin_index`) is already const
//     via Cell interior mutability, so all callers keep working.
//   * The formerly-private statics are public; nothing outside `select`
//     calls them.
#if RUSTYCPP_RUST
struct LoadBalancer {}

impl LoadBalancer {
    fn select<ClientVec>(strategy: LoadBalancingStrategy, clients: &ClientVec, state: &LoadBalancerState, rand_value: usize) -> usize {
        let pool_size: usize = lb_pool_size(clients);
        if pool_size == 0usize {
            return 0usize;
        }
        if strategy == LoadBalancingStrategy::ROUND_ROBIN {
            return LoadBalancer::select_round_robin(pool_size, state);
        }
        if strategy == LoadBalancingStrategy::LEAST_CONNECTIONS {
            return lb_select_least_connections(clients);
        }
        if strategy == LoadBalancingStrategy::LEAST_LATENCY {
            return lb_select_least_latency(clients);
        }
        LoadBalancer::select_random(pool_size, rand_value)
    }

    fn select_random(pool_size: usize, rand_value: usize) -> usize {
        rand_value % pool_size
    }

    fn select_round_robin(pool_size: usize, state: &LoadBalancerState) -> usize {
        state.next_round_robin_index(pool_size)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=load_balancer.balancer version=1 rust_sha256=7ccd3abea8f04387bedda62133c9f9ca1254c3a5ac8a7d5540fae9bd8280d75c*/
struct LoadBalancer;

struct LoadBalancer {

    template<typename ClientVec>
    static size_t select(LoadBalancingStrategy strategy, const ClientVec& clients, const LoadBalancerState& state, size_t rand_value);
    static size_t select_random(size_t pool_size, size_t rand_value);
    static size_t select_round_robin(size_t pool_size, const LoadBalancerState& state);
};


template<typename ClientVec>
size_t LoadBalancer::select(LoadBalancingStrategy strategy, const ClientVec& clients, const LoadBalancerState& state, size_t rand_value) {
    size_t pool_size = lb_pool_size(clients);
    if (rusty::detail::deref_if_pointer_like(pool_size) == static_cast<size_t>(0)) {
        return static_cast<size_t>(0);
    }
    if (rusty::detail::deref_if_pointer_like(strategy) == rusty::clone(LoadBalancingStrategy::ROUND_ROBIN)) {
        return LoadBalancer::select_round_robin(std::move(pool_size), state);
    }
    if (rusty::detail::deref_if_pointer_like(strategy) == rusty::clone(LoadBalancingStrategy::LEAST_CONNECTIONS)) {
        return lb_select_least_connections(clients);
    }
    if (rusty::detail::deref_if_pointer_like(strategy) == rusty::clone(LoadBalancingStrategy::LEAST_LATENCY)) {
        return lb_select_least_latency(clients);
    }
    return LoadBalancer::select_random(std::move(pool_size), std::move(rand_value));
}

size_t LoadBalancer::select_random(size_t pool_size, size_t rand_value) {
    return rusty::detail::deref_if_pointer_like(rand_value) % rusty::detail::deref_if_pointer_like(pool_size);
}

size_t LoadBalancer::select_round_robin(size_t pool_size, const LoadBalancerState& state) {
    return state.next_round_robin_index(std::move(pool_size));
}
/*RUSTYCPP:GEN-END id=load_balancer.balancer*/

} // export namespace rrr
