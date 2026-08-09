//! Selection policy shared by the legacy `rrr` client pool and native Rust.
//!
//! The C++ API intentionally remains structurally generic: callers may pass
//! either the production `rusty::Vec<rusty::Arc<Client>>` shape or test pools
//! such as `std::vector<std::shared_ptr<MockClient>>`. Rust has no structural
//! generic bounds, so the reserved, documentation-hidden [`cpp`] module below
//! supplies the corresponding nominal contract for native callers. The
//! transpiler consumes that contract but does not emit it as a C++ interface.
//!
//! Random selection keeps the existing caller-supplied `rand_value` seam. No
//! PRNG is owned here, which preserves deterministic tests and the legacy
//! client's choice of random source.

#![allow(non_camel_case_types, unsafe_code, unused_unsafe)]

use cpp::rusty as native;
use native::{ClientLike as _, MetricsLike as _};
use std::cell::Cell;

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
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

pub struct LoadBalancerState {
    pub round_robin_index_field: Cell<usize>,
}

impl LoadBalancerState {
    pub fn new() -> LoadBalancerState {
        LoadBalancerState {
            round_robin_index_field: Cell::<usize>::new(0usize),
        }
    }

    pub fn next_round_robin_index(&self, pool_size: usize) -> usize {
        if pool_size == 0usize {
            return 0usize;
        }
        let current = self.round_robin_index_field.get();
        self.round_robin_index_field
            .set((current + 1usize) % pool_size);
        current
    }

    pub fn reset(&self) {
        self.round_robin_index_field.set(0usize);
    }
}

pub fn lb_pool_size<ClientVec: native::ClientsLike>(clients: &ClientVec) -> usize {
    unsafe { native::len(clients) }
}

pub fn lb_select_least_connections<ClientVec: native::ClientsLike>(clients: &ClientVec) -> usize {
    let mut best_idx = 0usize;
    let mut min_pending = u64::MAX;
    let mut index = 0usize;
    while index < unsafe { native::len(clients) } {
        let pending = (*clients[index]).metrics().in_flight_requests();
        if pending < min_pending {
            min_pending = pending;
            best_idx = index;
        }
        index += 1usize;
    }
    best_idx
}

pub fn lb_select_least_latency<ClientVec: native::ClientsLike>(clients: &ClientVec) -> usize {
    let mut best_idx = 0usize;
    let mut min_latency = u64::MAX;
    let mut index = 0usize;
    while index < unsafe { native::len(clients) } {
        let average = (*clients[index]).metrics().avg_latency_us();
        let completed = (*clients[index]).metrics().requests_completed();
        if !(average == 0u64 && completed == 0u64) && average < min_latency {
            min_latency = average;
            best_idx = index;
        }
        index += 1usize;
    }
    best_idx
}

pub struct LoadBalancer {}

impl LoadBalancer {
    pub fn select<ClientVec: native::ClientsLike>(
        strategy: LoadBalancingStrategy,
        clients: &ClientVec,
        state: &LoadBalancerState,
        rand_value: usize,
    ) -> usize {
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

// The reserved `cpp` root is native-Rust type-checking metadata. rusty-cpp
// consumes its public traits as bounds and suppresses the module from C++.
#[doc(hidden)]
pub mod cpp {
    #[doc(hidden)]
    pub mod rusty {
        use core::ops::{Deref, Index};
        use std::sync::Arc;

        #[doc(hidden)]
        pub trait MetricsLike {
            fn in_flight_requests(&self) -> u64;
            fn avg_latency_us(&self) -> u64;
            fn requests_completed(&self) -> u64;
        }

        #[doc(hidden)]
        pub trait ClientLike {
            type Metrics: MetricsLike;
            fn metrics(&self) -> &Self::Metrics;
        }

        #[doc(hidden)]
        pub trait ClientsLike: Index<usize, Output = Self::Handle> {
            type Client: ClientLike;
            type Handle: Deref<Target = Self::Client>;
            fn len(&self) -> usize;
        }

        impl<T: ClientLike> ClientsLike for Vec<Arc<T>> {
            type Client = T;
            type Handle = Arc<T>;

            fn len(&self) -> usize {
                Vec::<Arc<T>>::len(self)
            }
        }

        /// Native implementation of the indexed `rusty::len` foreign call.
        ///
        /// # Safety
        ///
        /// This is marked unsafe solely because calls through the reserved
        /// `cpp` namespace model foreign C++ operations for the transpiler.
        /// The native implementation delegates to safe [`ClientsLike::len`].
        #[doc(hidden)]
        pub unsafe fn len<C: ClientsLike>(clients: &C) -> usize {
            clients.len()
        }
    }
}
