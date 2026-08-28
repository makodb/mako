use rrr::load_balancer::{
    load_balancing_strategy_to_string, LoadBalancer, LoadBalancerState, LoadBalancingStrategy,
};
use std::sync::Arc;

#[derive(Clone)]
struct Metrics {
    pending: u64,
    latency: u64,
    completed: u64,
}

impl rusty::LoadBalancerMetrics for Metrics {
    fn in_flight_requests(&self) -> u64 {
        self.pending
    }

    fn avg_latency_us(&self) -> u64 {
        self.latency
    }

    fn requests_completed(&self) -> u64 {
        self.completed
    }
}

struct Client {
    metrics: Metrics,
}

impl rusty::LoadBalancerClient for Client {
    type Metrics = Metrics;

    fn metrics(&self) -> &Metrics {
        &self.metrics
    }
}

fn clients() -> Vec<Arc<Client>> {
    vec![
        Arc::new(Client {
            metrics: Metrics {
                pending: 5,
                latency: 0,
                completed: 0,
            },
        }),
        Arc::new(Client {
            metrics: Metrics {
                pending: 2,
                latency: 80,
                completed: 10,
            },
        }),
        Arc::new(Client {
            metrics: Metrics {
                pending: 2,
                latency: 30,
                completed: 3,
            },
        }),
    ]
}

#[test]
fn state_and_strategy_boundaries_are_exact() {
    use LoadBalancingStrategy::*;
    assert_eq!(std::mem::size_of::<LoadBalancingStrategy>(), 4);
    assert_eq!(RANDOM as i32, 0);
    assert_eq!(ROUND_ROBIN as i32, 1);
    assert_eq!(LEAST_CONNECTIONS as i32, 2);
    assert_eq!(LEAST_LATENCY as i32, 3);
    assert_eq!(load_balancing_strategy_to_string(RANDOM), "RANDOM");
    assert_eq!(
        load_balancing_strategy_to_string(ROUND_ROBIN),
        "ROUND_ROBIN"
    );
    assert_eq!(
        load_balancing_strategy_to_string(LEAST_CONNECTIONS),
        "LEAST_CONNECTIONS"
    );
    assert_eq!(
        load_balancing_strategy_to_string(LEAST_LATENCY),
        "LEAST_LATENCY"
    );
    assert_eq!(
        std::mem::size_of::<LoadBalancerState>(),
        std::mem::size_of::<usize>()
    );
    assert_eq!(
        std::mem::align_of::<LoadBalancerState>(),
        std::mem::align_of::<usize>()
    );

    let state = LoadBalancerState::new();
    assert_eq!(state.next_round_robin_index(0), 0);
    assert_eq!(state.next_round_robin_index(3), 0);
    assert_eq!(state.next_round_robin_index(3), 1);
    state.round_robin_index_field.set(usize::MAX);
    assert_eq!(state.next_round_robin_index(3), usize::MAX);
    assert_eq!(state.round_robin_index_field.get(), 0);
    state.reset();
    assert_eq!(state.next_round_robin_index(3), 0);
}

#[test]
fn all_selection_strategies_preserve_behavior() {
    use LoadBalancingStrategy::*;
    let clients = clients();
    let state = LoadBalancerState::new();
    assert_eq!(LoadBalancer::select(RANDOM, &clients, &state, 8), 2);
    assert_eq!(
        LoadBalancer::select(LEAST_CONNECTIONS, &clients, &state, 0),
        1
    );
    assert_eq!(LoadBalancer::select(LEAST_LATENCY, &clients, &state, 0), 2);
    state.reset();
    assert_eq!(LoadBalancer::select(ROUND_ROBIN, &clients, &state, 0), 0);
    assert_eq!(LoadBalancer::select(ROUND_ROBIN, &clients, &state, 0), 1);
    let empty: Vec<Arc<Client>> = Vec::new();
    assert_eq!(LoadBalancer::select(RANDOM, &empty, &state, 9), 0);
}
