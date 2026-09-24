// Canonical Rust source for the srpc.load_balancer module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use rusty::{LoadBalancerClient as _, LoadBalancerMetrics as _};
use std::cell::Cell;
use std::ops::{Deref, Index};

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(i32)]
pub enum LoadBalancingStrategy {
    RANDOM = 0,
    ROUND_ROBIN = 1,
    LEAST_CONNECTIONS = 2,
    LEAST_LATENCY = 3,
}

#[allow(unreachable_patterns)]
pub fn load_balancing_strategy_to_string(strategy: LoadBalancingStrategy) -> &'static str {
    match strategy {
        LoadBalancingStrategy::RANDOM => "RANDOM",
        LoadBalancingStrategy::ROUND_ROBIN => "ROUND_ROBIN",
        LoadBalancingStrategy::LEAST_CONNECTIONS => "LEAST_CONNECTIONS",
        LoadBalancingStrategy::LEAST_LATENCY => "LEAST_LATENCY",
        _ => "UNKNOWN",
    }
}

#[repr(C)]
pub struct LoadBalancerState {
    pub round_robin_index_field: Cell<usize>,
}

impl LoadBalancerState {
    #[allow(clippy::new_without_default)]
    pub fn new() -> LoadBalancerState {
        LoadBalancerState {
            round_robin_index_field: Cell::new(0usize),
        }
    }

    pub fn next_round_robin_index(&self, pool_size: usize) -> usize {
        if pool_size == 0usize {
            return 0usize;
        }
        let current = self.round_robin_index_field.get();
        let next = current.wrapping_add(1usize) % pool_size;
        self.round_robin_index_field.set(next);
        current
    }

    pub fn reset(&self) {
        self.round_robin_index_field.set(0usize);
    }
}

pub fn lb_pool_size<ClientVec>(clients: &ClientVec) -> usize
where
    ClientVec: rusty::LoadBalancerClientVec,
    <ClientVec as Index<usize>>::Output: rusty::LoadBalancerClientHandle,
    <<ClientVec as Index<usize>>::Output as Deref>::Target: rusty::LoadBalancerClient,
{
    clients.len()
}

pub fn lb_select_least_connections<ClientVec>(clients: &ClientVec) -> usize
where
    ClientVec: rusty::LoadBalancerClientVec,
    <ClientVec as Index<usize>>::Output: rusty::LoadBalancerClientHandle,
    <<ClientVec as Index<usize>>::Output as Deref>::Target: rusty::LoadBalancerClient,
{
    let mut best_idx = 0usize;
    let mut min_pending = u64::MAX;
    let mut i = 0usize;
    while i < clients.len() {
        let pending = (*clients[i]).metrics().in_flight_requests();
        if pending < min_pending {
            min_pending = pending;
            best_idx = i;
        }
        i += 1usize;
    }
    best_idx
}

pub fn lb_select_least_latency<ClientVec>(clients: &ClientVec) -> usize
where
    ClientVec: rusty::LoadBalancerClientVec,
    <ClientVec as Index<usize>>::Output: rusty::LoadBalancerClientHandle,
    <<ClientVec as Index<usize>>::Output as Deref>::Target: rusty::LoadBalancerClient,
{
    let mut best_idx = 0usize;
    let mut min_latency = u64::MAX;
    let mut i = 0usize;
    while i < clients.len() {
        let avg_latency = (*clients[i]).metrics().avg_latency_us();
        let completed = (*clients[i]).metrics().requests_completed();
        if !(avg_latency == 0u64 && completed == 0u64) && avg_latency < min_latency {
            min_latency = avg_latency;
            best_idx = i;
        }
        i += 1usize;
    }
    best_idx
}

pub struct LoadBalancer {}

impl LoadBalancer {
    pub fn select<ClientVec>(
        strategy: LoadBalancingStrategy,
        clients: &ClientVec,
        state: &LoadBalancerState,
        rand_value: usize,
    ) -> usize
    where
        ClientVec: rusty::LoadBalancerClientVec,
        <ClientVec as Index<usize>>::Output: rusty::LoadBalancerClientHandle,
        <<ClientVec as Index<usize>>::Output as Deref>::Target: rusty::LoadBalancerClient,
    {
        let pool_size = lb_pool_size(clients);
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

    pub fn select_random(pool_size: usize, rand_value: usize) -> usize {
        rand_value % pool_size
    }

    pub fn select_round_robin(pool_size: usize, state: &LoadBalancerState) -> usize {
        state.next_round_robin_index(pool_size)
    }
}
