#include "cluster_bootstrap.h"

#include <stdlib.h>  // getenv
#include <string.h>  // strcmp
#include <string>
#include <vector>

#include "storage/abstract_db.h"           // abstract_db, abstract_ordered_index
#include "ordered_index_kv_store.h"        // OrderedIndexKvStore
#include "benchmarks/benchmark_config.h"   // BenchmarkConfig
#include "shard_data_plane.h"              // make_engine_shard_data (engine-side factory TU)
#include "standalone_index.h"              // registry-free system-table indexes

import cluster;   // config/sharding metadata module (was #include "cluster/...")

#include "deptran/config_kv_service.h"     // ConfigKvServiceImpl, make_config_read_fn,
                                           // ConfigKvServiceProxy
#include "shard_data_service.h"            // ShardDataServiceImpl / RemoteShardData / proxies

#include "rrr/rrr.hpp"                      // rrr::Server / Client / PollThread, Log_*
#include <rusty/mutex.hpp>                  // rusty::Mutex (one Migrate at a time)

namespace janus {
namespace {

// The config-read service listens at shard 0's MAKO transport port + this
// delta (server bind and client connect derive it identically from the
// SHARED mako config, which names every shard on every node -- unlike the
// per-shard deptran paxos config, which cannot address peers and does not
// even exist on the no-replication bed). Mako shard port windows are 100
// apart and mako itself binds shard_port + id with id <= ~20, so small
// deltas in the 25..99 gap are free: 50 = xproc demo, 60 = the data
// plane, 70 = this. (History: the delta was +20000 off the deptran leader
// port, which overflowed 65535 for most randomized CI port bases and
// silently disabled the service.)
constexpr int kConfigKvPortDelta = 70;

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
// (shard-0 leader's local config store is a leaked engine-gated KvStore,
//  created in StartShard0Leader via mako::make_engine_kv_store)
rusty::Option<rusty::Box<RemoteKvStore>> g_cfg_kv_remote;        // other nodes
rusty::Option<rusty::Box<ConfigManager>> g_cfg_cm;
rusty::Option<rusty::Box<ConfigWatcher>> g_cfg_watcher;
rusty::Option<rusty::Box<ShardMaster>> g_shard_master;   // shard-0 leader only: THE migration coordinator

// ---- migratable data plane + admin trigger ----
// Every shard LEADER serves its ShardDataService (the migration data plane, over
// a real engine-backed index) at mako_shard_port + kShardDataPortDelta; shard 0's
// leader additionally serves the MigrationAdmin trigger on the SAME server.
// Addresses derive from the MAKO transport config (BenchmarkConfig's
// transport::Configuration), which every process shares and which names every
// shard -- the deptran paxos config is per-shard and cannot address peers. The
// delta sits inside each shard's mako port window (mako binds shard_port + id,
// id <= ~20; windows are 100+ apart) and clear of the +50 the xproc demo uses.
constexpr int kShardDataPortDelta = 60;
// The default migratable non-txn KV table (pre-created so the data plane is
// never empty); any OTHER table named by a Migrate is created on demand by the
// catalog on both sides.
constexpr const char* kMigratableTable = "__mako_kv__";
// FIXED table id for the config store. System-table indexes are created
// STANDALONE (mako::make_standalone_index / the catalog) -- NOT via
// abstract_db::open_index -- because open_index assigns per-process sequential
// ids: bootstrap-opened tables would shift every workload table id after them,
// asymmetrically across shards (only shard 0 opens the config store), breaking
// cross-shard table-id arithmetic (observed live: ~100% remote aborts on both
// shards). 9001 + the catalog's 9100+ sit far outside the per-shard 200-id
// workload windows.
constexpr long kConfigStoreTableId = 9001;
ShardDataCatalog* g_data_catalog = nullptr;               // this shard's named tables (leaked)
abstract_db* g_dp_db = nullptr;                           // for engine thread registration
rusty::Option<rusty::Arc<rrr::PollThread>> g_data_poll;   // data-plane server poll
rrr::Server* g_data_server = nullptr;
rusty::Option<rusty::Arc<rrr::PollThread>> g_admin_poll;  // client polls for remote participants
rusty::Mutex<int> g_admin_mu{0};                          // serialize Migrate calls

// @safe - env-var read
bool cluster_config_enabled() {
    const char* v = getenv("MAKO_CLUSTER_CONFIG");
    return v != nullptr && strcmp(v, "1") == 0;
}

// Seed shard 0's config store from the shared mako topology so other
// nodes have a complete snapshot to read on their first poll. Blind
// puts; __version__ is bumped last (set_shard_count), so a reader that
// observes the new version sees every key of it. Shard identity comes
// from the MAKO transport config (host:port per shard) -- present in
// every mode; the deptran paxos config (per-shard, replicated-only) is
// deliberately not consulted, so this runs on the no-replication bed too.
// @unsafe - KvStore writes via ConfigManager
void SeedTopology(ConfigManager* cm, uint32_t nshards) {
    auto& bench = BenchmarkConfig::getInstance();
    for (uint32_t sid = 0; sid < nshards; ++sid) {
        auto s = bench.getConfig()->shard(static_cast<int>(sid), bench.getClusterRole());
        const std::string site = s.host + ":" + s.port;
        cm->set_shard_replicas(sid, {site});
        cm->set_shard_leader(sid, site);
        cm->set_shard_status(sid, "active");
    }
    // Routing strategy: "hash" (default) or "map" (per-table partition tables --
    // required for range migration cutovers to affect routing). Operator-chosen
    // via MAKO_SHARDING_MODE; anything but "map" falls back to hash.
    const char* mode_env = getenv("MAKO_SHARDING_MODE");
    const bool map_mode = (mode_env != nullptr && strcmp(mode_env, "map") == 0);
    cm->set_sharding_mode(map_mode ? "map" : "hash");
    cm->set_shard_count(nshards);  // version-bumping write, done last
}

// The mako-transport address of shard `sid`'s data-plane service ("host:port").
// Both sides derive it from the SHARED mako config, so it needs no discovery.
// @safe - reads the shared transport configuration
std::string data_plane_addr_of(uint32_t sid) {
    auto& bench = BenchmarkConfig::getInstance();
    auto s = bench.getConfig()->shard(static_cast<int>(sid), bench.getClusterRole());
    return s.host + ":" + std::to_string(atoi(s.port.c_str()) + kShardDataPortDelta);
}

// Operator-facing migration trigger: ONE Migrate RPC runs the full online range
// migration on the STANDING shard-0 ShardMaster (g_shard_master) against the
// shards' real data planes -- local (this process's engine index) or remote
// (RemoteShardData over the peer's ShardDataService). Sequence:
//   begin -> background copy (source live) -> lock (master freezes the SOURCE's
//   range: its write fence) -> catch-up copy over the now-stable range ->
//   checksum-gated final sync -> commit (source sheds the range; the cutover
//   publishes through the authoritative ConfigManager and version-bumps, so
//   every node's ConfigWatcher reroutes on its next reload).
// Runs inline on the rrr handler thread (a migration in flight blocks this
// server's other RPCs; acceptable for an operator-rate control plane).
// @unsafe - drives the storage engine, rrr clients, and the config store
class MigrationAdminImpl : public MigrationAdminService {
public:
    void Migrate(const RpcMigrateRequest& req, RpcMigrateResponse& resp,
                 rrr::DeferredReply defer) override {
        resp.moved = 0;
        std::string err = DoMigrate(req, &resp.moved);
        resp.ok = err.empty() ? 1 : 0;
        resp.msg = err.empty() ? "committed" : err;
        if (resp.ok) {
            Log_info("MigrationAdmin: committed [%s,%s) of '%s' shard %d -> %d (moved=%lld)",
                     req.lo.c_str(), req.hi.c_str(), req.table_name.c_str(),
                     req.src, req.dst, static_cast<long long>(resp.moved));
        } else {
            Log_warn("MigrationAdmin: REJECTED [%s,%s) of '%s' shard %d -> %d: %s",
                     req.lo.c_str(), req.hi.c_str(), req.table_name.c_str(),
                     req.src, req.dst, err.c_str());
        }
        defer.reply();
    }

private:
    // Returns "" on committed, else the failure detail.
    std::string DoMigrate(const RpcMigrateRequest& req, rrr::i64* moved) {
        // The commit publishes through the config store (engine writes), and the
        // local participant's ops run on THIS handler thread: engine-register it.
        mako::engine_register_this_thread(g_dp_db);
        auto lk = g_admin_mu.lock().unwrap();   // one migration at a time

        if (g_shard_master.is_none()) return "shard master not ready (shard-0 leader only)";
        ShardMaster* master = g_shard_master.as_ref().unwrap().get();
        if (g_cfg_cm.is_none()) return "config manager not ready";
        ConfigManager* cm = g_cfg_cm.as_ref().unwrap().get();
        if (req.src == req.dst) return "src == dst";
        if (req.src < 0 || req.dst < 0) return "negative shard id";
        if (master->is_migrating()) return "a migration is already in flight";
        if (cm->get_sharding_mode() != "map")
            return "sharding mode is not 'map' (start the cluster with "
                   "MAKO_SHARDING_MODE=map); a range cutover would not affect routing";

        std::string err = EnsureParticipant(master, static_cast<uint32_t>(req.src),
                                            req.table_name);
        if (!err.empty()) return err;
        err = EnsureParticipant(master, static_cast<uint32_t>(req.dst), req.table_name);
        if (!err.empty()) return err;

        // First migration of a table: seed its partition table with the whole
        // keyspace on the source, so the commit's split_and_reassign has an
        // authoritative partition to mutate.
        if (cm->get_partition_count(req.table_name) == 0) {
            if (!cm->seed_partition(req.table_name, static_cast<uint32_t>(req.src)))
                return "seed_partition failed";
        }

        if (!master->begin_migration(static_cast<uint32_t>(req.src),
                                     static_cast<uint32_t>(req.dst),
                                     req.table_name, req.lo, req.hi))
            return "begin_migration rejected";
        master->background_copy();     // Phase 1: copy, source still serving
        master->lock_range();          // Phase 2: master freezes the source's range
        master->background_copy();     // catch-up: the range is now write-fenced
        master->final_sync();          // Phase 3: checksum gate -> dest prepared
        if (!master->both_prepared()) {
            master->abort_migration();
            return "checksum verify failed -> aborted (source intact, unfrozen)";
        }
        if (!master->commit_migration())  // Phase 4: shed + publish cutover
            return "commit_migration failed";
        *moved = static_cast<rrr::i64>(master->shard_range_count(
            static_cast<uint32_t>(req.dst), req.lo, req.hi));
        return std::string();
    }

    // Attach shard `sid`'s data plane FOR `table` to the master (participants
    // are table-bound; attach_shard overwrites, so each migration binds the
    // handles for ITS table): this process's catalog entry for the local shard,
    // a table-bound RemoteShardData over the peer's ShardDataService otherwise.
    // Proxies are cached per shard, handles per (shard, table); all leaked
    // (process-lifetime). Returns "" or the failure detail.
    std::string EnsureParticipant(ShardMaster* master, uint32_t sid,
                                  const std::string& table) {
        auto& bench = BenchmarkConfig::getInstance();
        if (sid == static_cast<uint32_t>(bench.getShardIndex())) {
            if (g_data_catalog == nullptr) return "local data plane not up";
            ShardData* sd = g_data_catalog->get_or_create(table);
            if (sd == nullptr) return "cannot create local table " + table;
            master->attach_shard(sid, sd);
            return std::string();
        }
        const std::string hkey = std::to_string(sid) + "/" + table;
        auto hit = remotes_.find(hkey);
        if (hit != remotes_.end()) {
            master->attach_shard(sid, hit->second);
            return std::string();
        }
        ShardDataServiceProxy* proxy = nullptr;
        auto pit = proxies_.find(sid);
        if (pit != proxies_.end()) {
            proxy = pit->second;
        } else {
            std::string addr = data_plane_addr_of(sid);
            if (g_admin_poll.is_none()) {
                g_admin_poll = rusty::Some(rrr::PollThread::create());
            }
            auto* client = new rusty::Arc<rrr::Client>(      // leaked: process-lifetime
                rrr::Client::create(g_admin_poll.as_ref().unwrap().clone()));
            if ((*client)->connect(addr.c_str(), false) != 0) {
                return std::string("cannot reach shard ") + std::to_string(sid) +
                       " data plane at " + addr;
            }
            proxy = new ShardDataServiceProxy(const_cast<rrr::Client*>(client->get()));
            proxies_.emplace(sid, proxy);
            Log_info("MigrationAdmin: connected shard %u data plane at %s",
                     sid, addr.c_str());
        }
        auto* remote = new RemoteShardData(proxy, table);    // leaked participant
        remotes_.emplace(hkey, remote);
        master->attach_shard(sid, remote);
        return std::string();
    }

    std::map<uint32_t, ShardDataServiceProxy*> proxies_;   // per remote shard
    std::map<std::string, RemoteShardData*> remotes_;      // per (shard "/" table)
};

// Every shard LEADER: open the migratable index, optionally seed demo rows
// (test beds), and serve the ShardDataService over it. Shard 0's leader also
// registers the MigrationAdmin service on the same server.
// @unsafe - storage index open, RPC server bind
void StartShardDataPlane(abstract_db* db) {
    auto& bench = BenchmarkConfig::getInstance();
    const int shard = static_cast<int>(bench.getShardIndex());
    g_dp_db = db;
    g_data_catalog = mako::make_engine_shard_catalog(db);
    if (g_data_catalog == nullptr ||
        g_data_catalog->get_or_create(kMigratableTable) == nullptr) {
        Log_warn("ShardDataPlane: could not create catalog / '%s'", kMigratableTable);
        return;
    }
    // Test/demo seeding, from a fresh engine-registered thread (never this one).
    // MAKO_MIGRATE_SEED_TABLE picks the table (default the standard KV table),
    // exercising on-demand creation of a custom-named table.
    const char* seed = getenv("MAKO_MIGRATE_SEED");
    if (seed != nullptr) {
        const char* ss = getenv("MAKO_MIGRATE_SEED_SHARD");
        const int seed_shard = (ss != nullptr) ? atoi(ss) : 1;
        const char* st = getenv("MAKO_MIGRATE_SEED_TABLE");
        const std::string seed_table = (st != nullptr) ? st : kMigratableTable;
        if (shard == seed_shard) {
            ShardData* sd = g_data_catalog->get_or_create(seed_table);
            if (sd != nullptr) {
                mako::seed_shard_data(sd, atoi(seed));
                Log_info("ShardDataPlane: seeded %d demo rows into '%s' on shard %d",
                         atoi(seed), seed_table.c_str(), shard);
            }
        }
    }
    auto me = bench.getConfig()->shard(shard, bench.getClusterRole());
    std::string bind_addr = "0.0.0.0:" +
        std::to_string(atoi(me.port.c_str()) + kShardDataPortDelta);
    g_data_poll = rusty::Some(rrr::PollThread::create());
    g_data_server = new rrr::Server(rusty::Some(g_data_poll.as_ref().unwrap().clone()));
    g_data_server->reg_service(rusty::make_box<ShardDataServiceImpl>(g_data_catalog));
    if (shard == 0) {
        g_data_server->reg_service(rusty::make_box<MigrationAdminImpl>());
    }
    if (g_data_server->start(bind_addr.c_str()) != 0) {
        Log_warn("ShardDataPlane: failed to bind %s", bind_addr.c_str());
        return;
    }
    Log_info("ShardDataPlane: shard %d data plane listening on %s%s",
             shard, bind_addr.c_str(),
             shard == 0 ? " (with MigrationAdmin)" : "");
}

// Shard 0's leader: owns the config store, serves reads, and keeps its
// own routing cache fresh from the local store (no self-RPC).
// @unsafe - storage index open, RPC server bind, background thread
void StartShard0Leader(abstract_db* db, uint32_t nshards) {
    // Standalone (fixed-id) config index -- see kConfigStoreTableId above for
    // why this must not go through open_index.
    ::FullOrderedIndex* idx =
        mako::make_standalone_index("__mako_config__", kConfigStoreTableId);
    if (idx == nullptr) {
        Log_warn("BootstrapClusterConfig: could not create __mako_config__ index");
        return;
    }
    // Engine-gated store: the ConfigWatcher poll thread and the ConfigKvService
    // rrr handler thread read this index from otherwise-unregistered threads;
    // ungated, those reads segfault the leader (observed live once the config
    // server's bind was fixed and the watcher actually started).
    KvStore* kv = mako::make_engine_kv_store(db, idx);   // leaked: process-lifetime
    if (kv == nullptr) {
        Log_warn("BootstrapClusterConfig: could not wrap __mako_config__ store");
        return;
    }

    g_cfg_cm = rusty::Some(rusty::make_box<ConfigManager>(kv));
    ConfigManager* cm = g_cfg_cm.as_ref().unwrap().get();
    SeedTopology(cm, nshards);

    // The long-lived migration coordinator on the master shard (shard 0): the ONE
    // ShardMaster every migration in the cluster goes through, over this shard's
    // authoritative ConfigManager + the routing cache. Each shard's data plane
    // registers as a participant; the MigrationAdmin trigger drives the 2PC, and
    // commit publishes the [lo,hi)->owner cutover through the ConfigManager,
    // which the ConfigWatcher below then propagates cluster-wide. Created BEFORE
    // the config server so an RPC bind failure can never take the master with it.
    g_shard_master = rusty::Some(rusty::make_box<ShardMaster>(
        ShardMaster::new_(cm, &get_cluster_config())));
    // Participants are table-bound and attach per migration (the admin handler
    // binds local catalog entries / remote table-bound proxies for each call).
    Log_info("BootstrapClusterConfig: shard-0 ShardMaster (migration coordinator) ready");

    // Dedicated RPC server for config reads, at this shard's mako port + delta
    // (same shared-config derivation the data plane uses).
    auto& bench = BenchmarkConfig::getInstance();
    auto me = bench.getConfig()->shard(0, bench.getClusterRole());
    std::string bind_addr =
        "0.0.0.0:" + std::to_string(atoi(me.port.c_str()) + kConfigKvPortDelta);
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
}

// Every other node (shard-0 followers + all non-zero shards): read shard
// 0's config over RPC and watch it for changes.
// @unsafe - RPC client connect, background thread
void StartRemoteWatcher() {
    // Shard 0's config service address from the SHARED mako config -- every
    // process can name shard 0 (the old per-shard deptran paxos config could
    // not: each process resolved partition 0 within its OWN file and dialed
    // its own port + delta, where nothing listens).
    auto& bench = BenchmarkConfig::getInstance();
    auto leader0 = bench.getConfig()->shard(0, bench.getClusterRole());
    std::string addr =
        leader0.host + ":" + std::to_string(atoi(leader0.port.c_str()) + kConfigKvPortDelta);

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

    // Every shard LEADER serves its migratable data plane (non-txn writes are
    // leader-only, so followers/learners have no data plane to serve). Must run
    // before StartShard0Leader so the master can attach the local participant.
    if (bench.getLeaderConfig() != 0) {
        StartShardDataPlane(db);
    }

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
