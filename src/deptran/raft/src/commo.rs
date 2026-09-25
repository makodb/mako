// Raft's communicator, in Rust. rustc compiles this into libraft.a; nothing
// here is transpiled, so edit it directly.
//
// WHY THIS EXISTS. `RaftServerBase` has 48 fields and exactly one of them is
// not `Send`: `commo_: *mut rusty::Communicator` (server_h.rs:1775). That
// single raw pointer is what stops the Raft server satisfying
// `trait Service: Send + Sync`, which the Rust srpc lane requires of anything
// it will dispatch to. Replacing it with a type Rust owns is the point.
//
// WHY Arc<Mutex<Client>> AND NOT Arc<Client>. srpc's Client is `Send` but not
// `Sync`: it holds RefCell<Option<Arc<ClientConnection>>>, Cell<bool> and
// Cell<i64> (rpc/client.rs:1550) because it is built for single-threaded fiber
// use. C++ solves this the same way -- RpcPeer owns its client behind
// `request_mutex_` (communicator.h:36) and locks per request. So this is the
// existing locking granularity expressed in Rust, not a new cost: one
// uncontended mutex per peer per send, exactly as today.
//
// WHY THE SENDING SIDE CAN MOVE ON ITS OWN. A Rust-lane client and a C++-lane
// server speak the same wire: the rpc ids and field order are generated from
// one rcc_rpc.rpc, and src/deptran/raft/tests/rpc_wire_golden.rs pins the
// exact bytes. So a node whose commo is Rust interoperates with peers still
// serving from C++, and the cutover does not have to be simultaneous.
//
// WHAT IS NOT HERE, AND WHY. Of the five operations Raft reaches the
// communicator through, two hand over C++-owned objects and stay as
// `extern "C"` kernels:
//
//   BroadcastVote        returns RaftVoteQuorumPtr, a 16-byte C++ carrier with
//                        its own destructor kernel (rusty-rustc:644, :753)
//   SendInstallSnapshot  takes RaftSnapshotManagerPtr; SnapshotManager's
//                        virtuals stay C++ under every variant considered
//                        (snapshot_manager.hpp:164-225)
//
// The other three -- the network-enabled flag, the poll-thread handle and the
// AppendEntries send -- are here.

// THIS IS NOT THE PEER TABLE. That is `PeerRegistry`, in
// src/deptran/communicator.h, written once as Rust and compiled into both
// lanes; it is partition-aware, it is what `Communicator` holds, and it is
// what both engines run against today (stage 3d of
// docs/migration/raft/commo-service-rpc-plan.md).
//
// What THIS file is for is the one thing PeerRegistry deliberately does not
// do: hold RUST-LANE clients. PeerRegistry carries
// `std::shared_ptr<RpcPeer>` as opaque bytes and never follows one, because
// the connections belong to the C++ lane. Moving them is the lane swap, 3e.
//
// So treat the type below as a sketch of that destination and not as
// anything to build on as it stands. In particular `peers_except` is NOT
// `PeersForPartition(par_id)` -- it has no partition dimension at all -- and
// when 3e lands this should be rebuilt on PeerRegistry's shape rather than
// extended. Nothing calls it; the tests assert only that the shape is
// `Send + Sync`, which is the property 3e needs from it.

#![allow(dead_code)]

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::rpc::{AppendEntriesRequest, EmptyAppendEntriesRequest, VoteRequest};

/// One peer's client, behind the same per-peer lock C++ uses.
pub struct Peer {
    pub site_id: u16,
    client: Mutex<srpc::client::Client>,
}

impl Peer {
    pub fn new(site_id: u16, client: srpc::client::Client) -> Self {
        Self { site_id, client: Mutex::new(client) }
    }

    /// Run `f` with the client locked. Mirrors RpcPeer::WithClient, whose
    /// lock_guard has the same extent (communicator.h:36).
    pub fn with_client<R>(&self, f: impl FnOnce(&srpc::client::Client) -> R) -> Option<R> {
        self.client.lock().ok().map(|guard| f(&guard))
    }
}

/// The Rust half of Raft's communicator.
///
/// Holds what Rust can own outright. The quorum and snapshot paths still go
/// through kernels, so this does not replace `janus::Communicator` -- it takes
/// over the parts that were only ever a raw pointer's worth of indirection.
pub struct RaftCommo {
    peers: HashMap<u16, Arc<Peer>>,
    /// Mirrors Communicator::network_enabled_ (communicator.h:94), which is
    /// an atomic there too. The lab suite toggles this to simulate partitions.
    network_enabled: AtomicBool,
}

impl Default for RaftCommo {
    fn default() -> Self {
        Self::new()
    }
}

impl RaftCommo {
    pub fn new() -> Self {
        Self { peers: HashMap::new(), network_enabled: AtomicBool::new(true) }
    }

    pub fn add_peer(&mut self, peer: Arc<Peer>) {
        self.peers.insert(peer.site_id, peer);
    }

    pub fn peer(&self, site_id: u16) -> Option<&Arc<Peer>> {
        self.peers.get(&site_id)
    }

    pub fn peer_count(&self) -> usize {
        self.peers.len()
    }

    /// Every peer except this site, which is who a broadcast reaches.
    pub fn peers_except(&self, self_site_id: u16) -> Vec<Arc<Peer>> {
        self.peers
            .iter()
            .filter(|(id, _)| **id != self_site_id)
            .map(|(_, peer)| peer.clone())
            .collect()
    }

    // Release/Acquire rather than SeqCst: this is a single flag with no
    // ordering relationship to other state, and it is what the C++ side uses
    // (communicator.h:73, :76).
    pub fn set_network_enabled(&self, enabled: bool) {
        self.network_enabled.store(enabled, Ordering::Release);
    }

    pub fn network_enabled(&self) -> bool {
        self.network_enabled.load(Ordering::Acquire)
    }
}

/// Requests a Raft node sends. Kept as one enum so the send path has a single
/// place to consult `network_enabled` before touching a client.
pub enum Outbound {
    Vote(VoteRequest),
    AppendEntries(AppendEntriesRequest),
    EmptyAppendEntries(EmptyAppendEntriesRequest),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commo_is_send_and_sync() {
        // The entire reason this type exists: commo_ is the one field making
        // RaftServerBase !Send, so a replacement that is not Send + Sync buys
        // nothing. Asserted here rather than discovered at the use site.
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<RaftCommo>();
        assert_send_sync::<Arc<Peer>>();
    }

    #[test]
    fn network_enabled_defaults_on_and_toggles() {
        let commo = RaftCommo::new();
        assert!(commo.network_enabled(), "a fresh commo must be connected");
        commo.set_network_enabled(false);
        assert!(!commo.network_enabled());
        commo.set_network_enabled(true);
        assert!(commo.network_enabled());
    }

    #[test]
    fn a_fresh_commo_has_no_peers_to_broadcast_to() {
        let commo = RaftCommo::new();
        assert_eq!(commo.peer_count(), 0);
        assert!(commo.peers_except(1).is_empty());
        assert!(commo.peer(1).is_none());
    }
}
