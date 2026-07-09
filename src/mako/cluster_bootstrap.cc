#include "cluster_bootstrap.h"

#include <stdlib.h>  // getenv
#include <string.h>  // strcmp
#include <string>
#include <vector>

#include "storage/abstract_db.h"           // abstract_db, abstract_ordered_index
#include "ordered_index_kv_store.h"        // OrderedIndexKvStore
#include "benchmarks/benchmark_config.h"   // BenchmarkConfig

import cluster;   // config/sharding metadata module (was #include "cluster/...")

#include "deptran/config.h"                // Config
#include "deptran/config_kv_service.h"     // ConfigKvServiceImpl, make_config_read_fn,
                                           // ConfigKvServiceProxy

#include "rrr/rrr.hpp"                      // rrr::Server / Client / PollThread, Log_*

namespace janus {
namespace {

// The config-read service listens on shard-0 leader's base port + this
// delta. 10000 is already the heartbeat control-server delta
// (PaxosWorker/RaftWorker CtrlPortDelta), so a distinct offset avoids
// colliding with it. Both server bind and client connect use the same
// delta, so they stay symmetric.
constexpr int kConfigKvPortDelta = 20000;

// How often a node re-polls shard 0 for a config-version change.
constexpr uint64_t kConfigPollIntervalMs = 1000;

// ---- Process-lifetime singletons (this wiring runs once per node) ------
// File scope, mirroring config_node_init.cc's g_config_* pattern. The
// Boxes give stable addresses for the raw-pointer cross-references below
// (ConfigManager borrows the KvStore; ConfigWatcher borrows both the
// ConfigManager and the routing cache).
rusty::Option<rusty::Arc<rrr::PollThread>> g_cfg_poll;
rrr::Server* g_cfg_server = nullptr;                          // shard-0 leader only
rusty::Option<rusty::Arc<rrr::Client>> g_cfg_client;         // other nodes only
ConfigKvServiceProxy* g_cfg_proxy = nullptr;                 // other nodes only
rusty::Option<rusty::Box<OrderedIndexKvStore>> g_cfg_kv_local;   // shard-0 leader
rusty::Option<rusty::Box<RemoteKvStore>> g_cfg_kv_remote;        // other nodes
rusty::Option<rusty::Box<ConfigManager>> g_cfg_cm;
rusty::Option<rusty::Box<ConfigWatcher>> g_cfg_watcher;
rusty::Option<rusty::Box<ShardMaster>> g_shard_master;   // shard-0 leader only: THE migration coordinator

// @safe - env-var read
bool cluster_config_enabled() {
    const char* v = getenv("MAKO_CLUSTER_CONFIG");
    return v != nullptr && strcmp(v, "1") == 0;
}

// Seed shard 0's config store from the static YAML topology so other
// nodes have a complete snapshot to read on their first poll. Blind
// puts; __version__ is bumped last (set_shard_count), so a reader that
// observes the new version sees every key of it.
// @unsafe - KvStore writes via ConfigManager
void SeedTopology(ConfigManager* cm, uint32_t nshards) {
    Config* cfg = Config::GetConfig();
    for (uint32_t sid = 0; sid < nshards; ++sid) {
        std::vector<std::string> replica_names;
        for (auto& site : cfg->SitesByPartitionId(sid)) {
            replica_names.push_back(site.name);
        }
        cm->set_shard_replicas(sid, replica_names);
        cm->set_shard_leader(sid, cfg->LeaderSiteByPartitionId(sid).name);
        cm->set_shard_status(sid, "active");
    }
    cm->set_sharding_mode("hash");
    cm->set_shard_count(nshards);  // version-bumping write, done last
}

// Shard 0's leader: owns the config store, serves reads, and keeps its
// own routing cache fresh from the local store (no self-RPC).
// @unsafe - storage index open, RPC server bind, background thread
void StartShard0Leader(abstract_db* db, uint32_t nshards) {
    abstract_ordered_index* idx = db->open_index("__mako_config__", /*shard_index=*/0);
    if (idx == nullptr) {
        Log_warn("BootstrapClusterConfig: could not open __mako_config__ index");
        return;
    }
    g_cfg_kv_local = rusty::Some(rusty::make_box<OrderedIndexKvStore>(idx));
    KvStore* kv = g_cfg_kv_local.as_ref().unwrap().get();

    g_cfg_cm = rusty::Some(rusty::make_box<ConfigManager>(kv));
    ConfigManager* cm = g_cfg_cm.as_ref().unwrap().get();
    SeedTopology(cm, nshards);

    // Dedicated RPC server for config reads (config_node_init.cc pattern).
    Config::SiteInfo me = Config::GetConfig()->LeaderSiteByPartitionId(0);
    std::string bind_addr = "0.0.0.0:" + std::to_string(me.port + kConfigKvPortDelta);
    g_cfg_poll = rusty::Some(rrr::PollThread::create());
    g_cfg_server = new rrr::Server(rusty::Some(g_cfg_poll.as_ref().unwrap().clone()));
    g_cfg_server->reg_service(rusty::make_box<ConfigKvServiceImpl>(kv));
    if (g_cfg_server->start(bind_addr.c_str()) != 0) {
        Log_warn("BootstrapClusterConfig: config server failed to bind %s",
                 bind_addr.c_str());
        return;
    }
    Log_info("BootstrapClusterConfig: shard-0 config service listening on %s",
             bind_addr.c_str());

    g_cfg_watcher = rusty::Some(rusty::make_box<ConfigWatcher>(
        ConfigWatcher::new_(cm, &get_cluster_config(), kConfigPollIntervalMs)));
    g_cfg_watcher.as_ref().unwrap()->poll();   // prime the cache immediately
    g_cfg_watcher.as_ref().unwrap()->start();

    // The long-lived migration coordinator on the master shard (shard 0): the ONE
    // ShardMaster every migration in the cluster goes through, over this shard's
    // authoritative ConfigManager + the routing cache. Each shard's data plane
    // registers as a participant; an admin trigger drives the 2PC, and commit
    // publishes the [lo,hi)->owner cutover through the ConfigManager, which the
    // ConfigWatcher above then propagates cluster-wide.
    g_shard_master = rusty::Some(rusty::make_box<ShardMaster>(
        ShardMaster::new_(cm, &get_cluster_config())));
    Log_info("BootstrapClusterConfig: shard-0 ShardMaster (migration coordinator) ready");
}

// Every other node (shard-0 followers + all non-zero shards): read shard
// 0's config over RPC and watch it for changes.
// @unsafe - RPC client connect, background thread
void StartRemoteWatcher() {
    Config::SiteInfo leader0 = Config::GetConfig()->LeaderSiteByPartitionId(0);
    std::string addr = leader0.GetHostAddr(kConfigKvPortDelta);

    g_cfg_poll = rusty::Some(rrr::PollThread::create());
    g_cfg_client = rusty::Some(rrr::Client::create(g_cfg_poll.as_ref().unwrap().clone()));
    if (g_cfg_client.as_ref().unwrap()->connect(addr.c_str(), false) != 0) {
        Log_warn("BootstrapClusterConfig: could not connect to shard-0 config at %s",
                 addr.c_str());
        return;
    }
    g_cfg_proxy = new ConfigKvServiceProxy(
        const_cast<rrr::Client*>(g_cfg_client.as_ref().unwrap().get()));

    g_cfg_kv_remote = rusty::Some(rusty::make_box<RemoteKvStore>(
        make_config_read_fn(g_cfg_proxy)));
    g_cfg_cm = rusty::Some(rusty::make_box<ConfigManager>(
        g_cfg_kv_remote.as_ref().unwrap().get()));
    // Watcher retries on each poll, so a not-yet-ready shard 0 is fine.
    g_cfg_watcher = rusty::Some(rusty::make_box<ConfigWatcher>(
        ConfigWatcher::new_(g_cfg_cm.as_ref().unwrap().get(), &get_cluster_config(), kConfigPollIntervalMs)));
    g_cfg_watcher.as_ref().unwrap()->start();
    Log_info("BootstrapClusterConfig: watching shard-0 config at %s", addr.c_str());
}

}  // namespace

// @unsafe - see per-branch helpers
void BootstrapClusterConfig(abstract_db* db) {
    static bool done = false;
    if (done) return;
    done = true;

    if (!cluster_config_enabled()) return;

    auto& bench = BenchmarkConfig::getInstance();
    const uint32_t nshards = static_cast<uint32_t>(bench.getNshards());
    if (nshards <= 1) return;  // sharding not in play; legacy routing stands

    const bool is_shard0_leader =
        (bench.getShardIndex() == 0) && (bench.getLeaderConfig() != 0);

    if (is_shard0_leader) {
        StartShard0Leader(db, nshards);
    } else {
        StartRemoteWatcher();
    }
}

// The long-lived ShardMaster on the master shard (shard 0's leader), or nullptr
// on any other node -- only shard 0 hosts the coordinator. Valid after
// BootstrapClusterConfig() has run. Future admin/migration triggers reach it by
// declaring, under `import cluster;`:
//     namespace janus { class ShardMaster; ShardMaster* get_shard_master(); }
// then registering each shard's ShardData participant and driving begin/copy/
// lock/final_sync/commit. Kept out of cluster_bootstrap.h so that header stays
// free of the cluster module import.
// @unsafe - borrowed pointer into the process-lifetime singleton
ShardMaster* get_shard_master() {
    if (g_shard_master.is_none()) return nullptr;
    return g_shard_master.as_ref().unwrap().get();
}

}  // namespace janus
