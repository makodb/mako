use srpc::rpc::load_balancer::cpp::rusty::{ClientLike, MetricsLike};
use srpc::rpc::load_balancer::{
    lb_pool_size, load_balancing_strategy_to_string, LoadBalancer, LoadBalancerState,
    LoadBalancingStrategy,
};
use std::sync::Arc;

struct Metrics {
    pending: u64,
    average: u64,
    completed: u64,
}

impl MetricsLike for Metrics {
    fn in_flight_requests(&self) -> u64 {
        self.pending
    }

    fn avg_latency_us(&self) -> u64 {
        self.average
    }

    fn requests_completed(&self) -> u64 {
        self.completed
    }
}

struct Client {
    metrics: Metrics,
}

impl ClientLike for Client {
    type Metrics = Metrics;

    fn metrics(&self) -> &Metrics {
        &self.metrics
    }
}

fn client(pending: u64, average: u64, completed: u64) -> Arc<Client> {
    Arc::new(Client {
        metrics: Metrics {
            pending,
            average,
            completed,
        },
    })
}

#[test]
fn strategy_discriminants_and_strings_are_stable() {
    assert_eq!(LoadBalancingStrategy::RANDOM as u8, 0);
    assert_eq!(LoadBalancingStrategy::ROUND_ROBIN as u8, 1);
    assert_eq!(LoadBalancingStrategy::LEAST_CONNECTIONS as u8, 2);
    assert_eq!(LoadBalancingStrategy::LEAST_LATENCY as u8, 3);

    assert_eq!(
        load_balancing_strategy_to_string(LoadBalancingStrategy::RANDOM),
        "RANDOM"
    );
    assert_eq!(
        load_balancing_strategy_to_string(LoadBalancingStrategy::ROUND_ROBIN),
        "ROUND_ROBIN"
    );
    assert_eq!(
        load_balancing_strategy_to_string(LoadBalancingStrategy::LEAST_CONNECTIONS),
        "LEAST_CONNECTIONS"
    );
    assert_eq!(
        load_balancing_strategy_to_string(LoadBalancingStrategy::LEAST_LATENCY),
        "LEAST_LATENCY"
    );
}

#[test]
fn round_robin_cycles_resets_and_handles_zero() {
    let state = LoadBalancerState::new();
    assert_eq!(state.next_round_robin_index(0), 0);
    assert_eq!(state.next_round_robin_index(3), 0);
    assert_eq!(state.next_round_robin_index(3), 1);
    assert_eq!(state.next_round_robin_index(3), 2);
    assert_eq!(state.next_round_robin_index(3), 0);
    state.reset();
    assert_eq!(state.next_round_robin_index(3), 0);
}

#[test]
fn empty_pool_returns_zero_for_every_strategy() {
    let clients: Vec<Arc<Client>> = Vec::new();
    let state = LoadBalancerState::new();

    assert_eq!(lb_pool_size(&clients), 0);

    for strategy in [
        LoadBalancingStrategy::RANDOM,
        LoadBalancingStrategy::ROUND_ROBIN,
        LoadBalancingStrategy::LEAST_CONNECTIONS,
        LoadBalancingStrategy::LEAST_LATENCY,
    ] {
        assert_eq!(LoadBalancer::select(strategy, &clients, &state, 17), 0);
    }
}

#[test]
fn caller_supplied_random_value_is_the_exact_selection_seam() {
    let clients = vec![client(0, 0, 0), client(0, 0, 0), client(0, 0, 0)];
    let state = LoadBalancerState::new();

    assert_eq!(
        LoadBalancer::select(LoadBalancingStrategy::RANDOM, &clients, &state, 8),
        2
    );
    assert_eq!(LoadBalancer::select_random(clients.len(), 9), 0);
}

#[test]
fn select_dispatches_round_robin_through_shared_state() {
    let clients = vec![client(0, 0, 0), client(0, 0, 0), client(0, 0, 0)];
    let state = LoadBalancerState::new();

    assert_eq!(
        LoadBalancer::select(LoadBalancingStrategy::ROUND_ROBIN, &clients, &state, 0,),
        0
    );
    assert_eq!(
        LoadBalancer::select(LoadBalancingStrategy::ROUND_ROBIN, &clients, &state, 0,),
        1
    );
}

#[test]
fn least_connections_chooses_the_first_minimum() {
    let clients = vec![
        client(8, 0, 0),
        client(2, 0, 0),
        client(2, 0, 0),
        client(5, 0, 0),
    ];
    let state = LoadBalancerState::new();

    assert_eq!(
        LoadBalancer::select(
            LoadBalancingStrategy::LEAST_CONNECTIONS,
            &clients,
            &state,
            0,
        ),
        1
    );
}

#[test]
fn least_latency_skips_only_clients_with_no_samples() {
    let clients = vec![
        client(0, 0, 0),
        client(0, 500, 1),
        client(0, 200, 1),
        client(0, 200, 1),
    ];
    let state = LoadBalancerState::new();

    assert_eq!(
        LoadBalancer::select(LoadBalancingStrategy::LEAST_LATENCY, &clients, &state, 0,),
        2,
        "the first sampled minimum wins"
    );

    let completed_zero_latency = vec![client(0, 50, 1), client(0, 0, 1)];
    assert_eq!(
        LoadBalancer::select(
            LoadBalancingStrategy::LEAST_LATENCY,
            &completed_zero_latency,
            &state,
            0,
        ),
        1,
        "zero latency is eligible once a request has completed"
    );

    let no_samples = vec![client(0, 0, 0), client(0, 0, 0)];
    assert_eq!(
        LoadBalancer::select(LoadBalancingStrategy::LEAST_LATENCY, &no_samples, &state, 0,),
        0
    );
}
