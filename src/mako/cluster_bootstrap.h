#pragma once

// Cluster-config runtime bootstrap.
//
// Ties the read-side cluster-config components together at node startup.
// Everything below this call already exists and is unit-tested in
// isolation (OrderedIndexKvStore, RemoteKvStore, ConfigManager,
// ConfigKvServiceImpl, ConfigWatcher, and the ClusterConfig routing
// cache the shard router consults); this is the one place that
// constructs and connects them on a live node.
//
// Forward-declared dependency only, so this header stays free of the
// storage-engine and RPC includes (those live in the .cc).

class abstract_db;

namespace janus {

// Wire the cluster-config read path. Call ONCE from init_env(), after
// the shard's RPC servers are up (post setup2()).
//
// Gated twice, so it is a no-op on the common CI paths:
//   1. the MAKO_CLUSTER_CONFIG env var must be "1", and
//   2. the cluster must have more than one shard.
// Single-shard / unsharded runs keep the legacy routing path untouched.
//
// When active, branches on this node's identity:
//   - Shard 0's leader opens its __mako_config__ index, wraps it in an
//     OrderedIndexKvStore, seeds it from the static YAML topology (shard
//     count + per-shard replicas/leader/status + default hash mode), and
//     stands up a dedicated ConfigKvService RPC server (on the leader
//     port + kConfigKvPortDelta) so other nodes can read config keys.
//   - Every other node connects to shard 0's leader, wraps that RPC in a
//     RemoteKvStore, and starts a ConfigWatcher.
// Both then run a ConfigWatcher that keeps janus::get_cluster_config()
// — the routing cache — refreshed (shard 0's leader watches its local
// store; everyone else watches shard 0 over RPC).
//
// Functional verification requires a live multi-shard cluster (Docker
// CI, e.g. `./docker_build.sh ci shard2Replication` with the env var
// set); this build only compile-checks the wiring.
//
// @unsafe - RPC I/O, storage index open, background thread creation.
void BootstrapClusterConfig(abstract_db* db);

}  // namespace janus
