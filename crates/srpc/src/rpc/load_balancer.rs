//! Peer selection — the port of `src/rrr/rpc/load_balancer.cpp`.
//!
//! The C++ version is a template over an opaque client vector that
//! reaches through each handle for `client->metrics()`. Here the
//! balancer states what it actually needs as a trait, [`Candidate`], so
//! the policies are testable against plain structs and the connection
//! type stays out of this module entirely.
//!
//! Two deliberate differences from the C++ surface:
//!   * selection returns `Option<usize>` — the C++ returned `0` for an
//!     empty pool, which a caller cannot tell from "picked peer 0";
//!   * an unrecognised strategy is not representable, so the
//!     `RANDOM`-and-default fall-through arm disappears.

use crate::base::rand::Rng;
use std::cell::Cell;

/// What the balancer needs to know about a candidate peer.
pub trait Candidate {
    /// Requests issued and not yet answered.
    fn in_flight_requests(&self) -> u64;
    /// Mean round-trip of completed requests, in microseconds.
    fn avg_latency_us(&self) -> u64;
    /// How many requests have completed — distinguishes "fast" from
    /// "never used", which both report zero latency.
    fn requests_completed(&self) -> u64;
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum LoadBalancingStrategy {
    Random = 0,
    RoundRobin = 1,
    LeastConnections = 2,
    LeastLatency = 3,
}

impl LoadBalancingStrategy {
    pub fn as_str(self) -> &'static str {
        match self {
            LoadBalancingStrategy::Random => "RANDOM",
            LoadBalancingStrategy::RoundRobin => "ROUND_ROBIN",
            LoadBalancingStrategy::LeastConnections => "LEAST_CONNECTIONS",
            LoadBalancingStrategy::LeastLatency => "LEAST_LATENCY",
        }
    }
}

impl std::fmt::Display for LoadBalancingStrategy {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// The rotation cursor. Only [`LoadBalancingStrategy::RoundRobin`] uses
/// it; the other policies are pure functions of the candidates.
pub struct LoadBalancerState {
    round_robin_index: Cell<usize>,
}

impl LoadBalancerState {
    pub fn new() -> LoadBalancerState {
        LoadBalancerState {
            round_robin_index: Cell::new(0),
        }
    }

    /// Current cursor, then advance it. Wraps on `pool_size`, so a pool
    /// that shrinks cannot leave the cursor out of range.
    pub fn next_round_robin_index(&self, pool_size: usize) -> usize {
        if pool_size == 0 {
            return 0;
        }
        let current = self.round_robin_index.get() % pool_size;
        self.round_robin_index.set((current + 1) % pool_size);
        current
    }

    pub fn reset(&self) {
        self.round_robin_index.set(0);
    }
}

impl Default for LoadBalancerState {
    fn default() -> LoadBalancerState {
        LoadBalancerState::new()
    }
}

/// Index of the peer with the fewest requests outstanding. Ties go to
/// the earliest, matching the C++ strict-less-than comparison.
fn select_least_connections<C: Candidate>(candidates: &[C]) -> usize {
    let mut best = 0;
    let mut min = u64::MAX;
    let mut i = 0;
    while i < candidates.len() {
        let pending = candidates[i].in_flight_requests();
        if pending < min {
            min = pending;
            best = i;
        }
        i += 1;
    }
    best
}

/// Index of the peer with the lowest mean latency.
///
/// Peers that have completed nothing are SKIPPED rather than treated as
/// zero-latency — otherwise an untried peer would always look perfect
/// and absorb the traffic. If every peer is untried the first is
/// chosen, which is how a fresh pool starts probing.
fn select_least_latency<C: Candidate>(candidates: &[C]) -> usize {
    let mut best = 0;
    let mut min = u64::MAX;
    let mut i = 0;
    while i < candidates.len() {
        let c = &candidates[i];
        let avg = c.avg_latency_us();
        if avg == 0 && c.requests_completed() == 0 {
            i += 1;
            continue;
        }
        if avg < min {
            min = avg;
            best = i;
        }
        i += 1;
    }
    best
}

/// Stateless policy dispatch.
pub struct LoadBalancer;

impl LoadBalancer {
    /// Pick a peer, or `None` when the pool is empty.
    pub fn select<C: Candidate>(
        strategy: LoadBalancingStrategy,
        candidates: &[C],
        state: &LoadBalancerState,
        rng: &Rng,
    ) -> Option<usize> {
        let pool_size = candidates.len();
        if pool_size == 0 {
            return None;
        }
        let idx = match strategy {
            LoadBalancingStrategy::RoundRobin => state.next_round_robin_index(pool_size),
            LoadBalancingStrategy::LeastConnections => select_least_connections(candidates),
            LoadBalancingStrategy::LeastLatency => select_least_latency(candidates),
            LoadBalancingStrategy::Random => rng.next_below(pool_size as u64) as usize,
        };
        Some(idx)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Peer {
        in_flight: u64,
        avg_latency: u64,
        completed: u64,
    }

    impl Peer {
        fn new(in_flight: u64, avg_latency: u64, completed: u64) -> Peer {
            Peer {
                in_flight,
                avg_latency,
                completed,
            }
        }
        /// A peer that has never served a request.
        fn untried() -> Peer {
            Peer::new(0, 0, 0)
        }
    }

    impl Candidate for Peer {
        fn in_flight_requests(&self) -> u64 {
            self.in_flight
        }
        fn avg_latency_us(&self) -> u64 {
            self.avg_latency
        }
        fn requests_completed(&self) -> u64 {
            self.completed
        }
    }

    fn pick(s: LoadBalancingStrategy, peers: &[Peer], st: &LoadBalancerState) -> Option<usize> {
        LoadBalancer::select(s, peers, st, &Rng::with_seed(1))
    }

    #[test]
    fn empty_pool_selects_nothing() {
        let st = LoadBalancerState::new();
        let empty: [Peer; 0] = [];
        let mut i = 0;
        let all = [
            LoadBalancingStrategy::Random,
            LoadBalancingStrategy::RoundRobin,
            LoadBalancingStrategy::LeastConnections,
            LoadBalancingStrategy::LeastLatency,
        ];
        while i < all.len() {
            assert_eq!(pick(all[i], &empty, &st), None, "{}", all[i]);
            i += 1;
        }
    }

    #[test]
    fn round_robin_rotates_and_wraps() {
        let st = LoadBalancerState::new();
        let peers = [Peer::untried(), Peer::untried(), Peer::untried()];
        assert_eq!(
            pick(LoadBalancingStrategy::RoundRobin, &peers, &st),
            Some(0)
        );
        assert_eq!(
            pick(LoadBalancingStrategy::RoundRobin, &peers, &st),
            Some(1)
        );
        assert_eq!(
            pick(LoadBalancingStrategy::RoundRobin, &peers, &st),
            Some(2)
        );
        assert_eq!(
            pick(LoadBalancingStrategy::RoundRobin, &peers, &st),
            Some(0)
        );
        st.reset();
        assert_eq!(
            pick(LoadBalancingStrategy::RoundRobin, &peers, &st),
            Some(0)
        );
    }

    #[test]
    fn round_robin_survives_a_shrinking_pool() {
        // The cursor is modulo'd on the CURRENT size, so removing peers
        // cannot hand back an out-of-range index.
        let st = LoadBalancerState::new();
        let five = [
            Peer::untried(),
            Peer::untried(),
            Peer::untried(),
            Peer::untried(),
            Peer::untried(),
        ];
        let mut i = 0;
        while i < 4 {
            pick(LoadBalancingStrategy::RoundRobin, &five, &st);
            i += 1;
        }
        let two = [Peer::untried(), Peer::untried()];
        let idx = pick(LoadBalancingStrategy::RoundRobin, &two, &st).unwrap();
        assert!(idx < 2, "index {idx} out of range for the shrunk pool");
    }

    #[test]
    fn least_connections_picks_the_idlest() {
        let st = LoadBalancerState::new();
        let peers = [
            Peer::new(7, 100, 10),
            Peer::new(2, 100, 10),
            Peer::new(5, 100, 10),
        ];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastConnections, &peers, &st),
            Some(1)
        );
    }

    #[test]
    fn least_connections_ties_go_to_the_earliest() {
        let st = LoadBalancerState::new();
        let peers = [Peer::new(3, 0, 0), Peer::new(3, 0, 0), Peer::new(3, 0, 0)];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastConnections, &peers, &st),
            Some(0)
        );
    }

    #[test]
    fn least_latency_picks_the_fastest() {
        let st = LoadBalancerState::new();
        let peers = [
            Peer::new(0, 900, 10),
            Peer::new(0, 120, 10),
            Peer::new(0, 450, 10),
        ];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastLatency, &peers, &st),
            Some(1)
        );
    }

    #[test]
    fn least_latency_skips_untried_peers() {
        // An untried peer reports zero latency. Counting that as
        // "fastest" would send it everything on the strength of no
        // evidence at all.
        let st = LoadBalancerState::new();
        let peers = [Peer::untried(), Peer::new(0, 300, 5), Peer::untried()];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastLatency, &peers, &st),
            Some(1),
            "the only measured peer wins"
        );
    }

    #[test]
    fn least_latency_on_a_fresh_pool_probes_the_first() {
        let st = LoadBalancerState::new();
        let peers = [Peer::untried(), Peer::untried()];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastLatency, &peers, &st),
            Some(0)
        );
    }

    #[test]
    fn a_genuinely_zero_latency_peer_still_counts() {
        // Zero latency WITH completions is a real measurement, not the
        // untried sentinel.
        let st = LoadBalancerState::new();
        let peers = [Peer::new(0, 50, 10), Peer::new(0, 0, 3)];
        assert_eq!(
            pick(LoadBalancingStrategy::LeastLatency, &peers, &st),
            Some(1)
        );
    }

    #[test]
    fn random_stays_in_range_and_spreads() {
        let st = LoadBalancerState::new();
        let peers = [
            Peer::untried(),
            Peer::untried(),
            Peer::untried(),
            Peer::untried(),
        ];
        let rng = Rng::with_seed(31337);
        let mut seen = [false; 4];
        let mut i = 0;
        while i < 2000 {
            let idx =
                LoadBalancer::select(LoadBalancingStrategy::Random, &peers, &st, &rng).unwrap();
            assert!(idx < 4, "index {idx} out of range");
            seen[idx] = true;
            i += 1;
        }
        let mut k = 0;
        while k < 4 {
            assert!(seen[k], "peer {k} never selected");
            k += 1;
        }
    }

    #[test]
    fn single_peer_pool_always_selects_it() {
        let st = LoadBalancerState::new();
        let peers = [Peer::new(99, 9999, 100)];
        let all = [
            LoadBalancingStrategy::Random,
            LoadBalancingStrategy::RoundRobin,
            LoadBalancingStrategy::LeastConnections,
            LoadBalancingStrategy::LeastLatency,
        ];
        let mut i = 0;
        while i < all.len() {
            assert_eq!(pick(all[i], &peers, &st), Some(0), "{}", all[i]);
            i += 1;
        }
    }

    #[test]
    fn strategy_discriminants_and_names() {
        assert_eq!(LoadBalancingStrategy::Random as u8, 0);
        assert_eq!(LoadBalancingStrategy::RoundRobin as u8, 1);
        assert_eq!(LoadBalancingStrategy::LeastConnections as u8, 2);
        assert_eq!(LoadBalancingStrategy::LeastLatency as u8, 3);
        assert_eq!(
            LoadBalancingStrategy::LeastLatency.to_string(),
            "LEAST_LATENCY"
        );
    }
}
