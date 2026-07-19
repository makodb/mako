#include "cluster_bootstrap.h"

#include <stdlib.h>  // getenv
#include <string.h>  // strcmp
#include <string>
#include <thread>    // async-migration job worker
#include <vector>

#include "storage/abstract_db.h"           // abstract_db, abstract_ordered_index
#include "ordered_index_kv_store.h"        // OrderedIndexKvStore
#include "benchmarks/benchmark_config.h"   // BenchmarkConfig
#include "benchmarks/tpcc_sharding.h"      // tpcc_seed_warehouse_partitions_if_master decl
#include "benchmarks/tpcc_warehouse_directory.h"   // parse_warehouse_spec
#include "lib/table_registry.h"            // warehouse_route_key (publish range)
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
// The MigrationAdmin control service gets its OWN rrr server (shard 0 only) at
// +61: its Migrate handler blocks for a whole migration, and during a
// destination-driven pull the DESTINATION connects BACK to shard 0's data plane
// for ScanRange -- if admin and data shared one server, the busy Migrate worker
// would starve that scan (observed live: pull timeout -> faulted -> abort).
constexpr int kMigrationAdminPortDelta = 61;
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
rusty::Option<rusty::Arc<rrr::PollThread>> g_admin_srv_poll;  // admin server poll (shard 0)
rrr::Server* g_admin_server = nullptr;
rusty::Option<rusty::Arc<rrr::PollThread>> g_admin_poll;  // client polls for remote participants
rusty::Mutex<int> g_admin_mu{0};                          // serialize Migrate calls
// Async-migration job state (see MigrationAdminImpl::Migrate): one job at a
// time; the terminal result is held until the poller consumes it.
struct MigJobState {
    bool running = false;
    bool has_result = false;
    bool ok = false;
    rrr::i64 moved = 0;
    std::string key;
    std::string msg;
};
rusty::Mutex<MigJobState> g_mig_job{MigJobState{}};

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
// A migration is an ASYNC JOB: rrr requests carry a ~1s client timeout, and a
// real (multi-second) migration blocking the handler had every big-table
// Migrate die client-side while the server kept going. The first Migrate for
// a (table,range,src,dst) spawns the job and replies "started"; the SAME call
// repeated polls it ("in progress" -> the terminal result, delivered once).
// mako_admin loops the call until terminal.
// @unsafe - drives the storage engine, rrr clients, and the config store
class MigrationAdminImpl : public MigrationAdminService {
public:
    void Migrate(const RpcMigrateRequest& req, RpcMigrateResponse& resp,
                 rrr::DeferredReply defer) override {
        // The key identifies the SUBMISSION, nonce included: an admin that
        // abandons a slow job at its own deadline and retries must not be
        // handed the abandoned attempt's terminal result (observed live:
        // attempt 2 "finished" in 100ms with attempt 1's verdict), and a
        // stale unconsumed result must not shadow a fresh submission.
        const std::string key = req.table_name + "|" + req.lo + "|" + req.hi + "|" +
                                std::to_string(req.src) + "|" + std::to_string(req.dst) +
                                "|" + std::to_string(req.nonce);
        resp.moved = 0;
        {
            auto st = g_mig_job.lock().unwrap();
            if ((*st).running) {
                resp.ok = 0;
                // Same submission: keep polling. Anything else (a foreign
                // spec, or a NEW attempt while the abandoned one still runs):
                // busy -- the admin polls until the slot frees, and its next
                // call starts the fresh job.
                resp.msg = ((*st).key == key) ? "in progress" : "busy";
                defer.reply();
                return;
            }
            if ((*st).has_result) {
                if ((*st).key == key) {
                    // Deliver the terminal result exactly once.
                    resp.ok = (*st).ok ? 1 : 0;
                    resp.moved = (*st).moved;
                    resp.msg = (*st).msg;
                    (*st).has_result = false;
                    defer.reply();
                    return;
                }
                // A different submission's unconsumed result: its poller
                // abandoned it. Discard and serve the new submission.
                (*st).has_result = false;
            }
            (*st).running = true;
            (*st).has_result = false;
            (*st).key = key;
        }
        RpcMigrateRequest job_req = req;
        // @unsafe { detached worker; process-lifetime service, job writes its
        // result back under g_mig_job before exiting }
        std::thread([this, job_req]() {
            rrr::i64 moved = 0;
            std::string err = DoMigrate(job_req, &moved);
            if (err.empty()) {
                Log_info("MigrationAdmin: committed [%s,%s) of '%s' shard %d -> %d (moved=%lld)",
                         job_req.lo.c_str(), job_req.hi.c_str(), job_req.table_name.c_str(),
                         job_req.src, job_req.dst, static_cast<long long>(moved));
            } else {
                Log_warn("MigrationAdmin: REJECTED [%s,%s) of '%s' shard %d -> %d: %s",
                         job_req.lo.c_str(), job_req.hi.c_str(), job_req.table_name.c_str(),
                         job_req.src, job_req.dst, err.c_str());
            }
            auto st = g_mig_job.lock().unwrap();
            (*st).running = false;
            (*st).has_result = true;
            (*st).ok = err.empty();
            (*st).moved = moved;
            (*st).msg = err.empty() ? "committed" : err;
        }).detach();
        resp.ok = 0;
        resp.msg = "started";
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

        // Un-latch stale faults from a PRIOR attempt on these cached
        // participants: the latch is per-(shard, table) and sticky, so one
        // congested chunk in attempt N would insta-abort attempt N+1 against
        // a perfectly healthy shard.
        {
            ShardData* s = ParticipantFor(static_cast<uint32_t>(req.src),
                                          req.table_name);
            ShardData* d = ParticipantFor(static_cast<uint32_t>(req.dst),
                                          req.table_name);
            if (s != nullptr) s->clear_faulted();
            if (d != nullptr) d->clear_faulted();
        }

        // Warehouse spec ("wh:<gwid>:<logical>"): the participants above bind
        // the physical per-warehouse WORKLOAD index on each side (whole-index
        // migration unit; the catalog resolves it through the warehouse
        // directory, materializing the destination's adopted index). What the
        // commit PUBLISHES is the LOGICAL table's warehouse routing segment --
        // physical row keys carry the shard-local warehouse id and never
        // appear in routing, so req.lo/req.hi are ignored for specs. The
        // participant side widens every range op to its whole index, so the
        // master's wrk-range drives copy/checksum/drop correctly.
        std::string publish_table = req.table_name;
        std::string publish_lo = req.lo;
        std::string publish_hi = req.hi;
        {
            int gwid = 0;
            std::string logical;
            if (mako::parse_warehouse_spec(req.table_name, &gwid, &logical)) {
                publish_table = logical;
                publish_lo = mako::warehouse_route_key(gwid);
                publish_hi = mako::warehouse_route_key(gwid + 1);
            }
        }

        // First migration of a table: seed its partition table with the whole
        // keyspace on the source, so the commit's split_and_reassign has an
        // authoritative partition to mutate. (Governed TPC-C tables are already
        // seeded by the workload; this covers ad-hoc tables.)
        if (cm->get_partition_count(publish_table) == 0) {
            if (!cm->seed_partition(publish_table, static_cast<uint32_t>(req.src)))
                return "seed_partition failed";
        }

        // Per-phase wall-clock: a return-leg job was observed running 21
        // MINUTES with zero output between start and terminal -- the hanging
        // phase was invisible. Every phase now names its cost.
        struct timespec ph_ts;
        clock_gettime(CLOCK_MONOTONIC, &ph_ts);
        long ph_last_ms = ph_ts.tv_sec * 1000L + ph_ts.tv_nsec / 1000000L;
        auto phase_ms = [&ph_last_ms]() {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long t = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
            const long d = t - ph_last_ms;
            ph_last_ms = t;
            return d;
        };

        if (!master->begin_migration(static_cast<uint32_t>(req.src),
                                     static_cast<uint32_t>(req.dst),
                                     publish_table, publish_lo, publish_hi))
            return "begin_migration rejected";
        // Everything from here to commit can run LOCAL engine scans on this
        // handler thread (local-participant copies, folds, diagnostics), and
        // those now carry a leaked-lock deadline that throws the retryable
        // abort (see ordered_index_shard_data.h) -- catch it at the bottom
        // so the migration aborts cleanly instead of unwinding the handler.
        try {
        master->background_copy();     // Phase 1: copy, source still serving
        Log_info("MigrationAdmin: '%s' %d->%d phase copy1 took %ld ms",
                 req.table_name.c_str(), req.src, req.dst, phase_ms());
        if (master->participants_faulted()) {
            // A participant's RPC path failed during the copy (e.g. one
            // congested chunk scan exhausting its client retries). Marching
            // on would burn the whole lock+drain deadline against a
            // participant that cannot answer; abort NOW, before the range is
            // ever fenced, so the admin gets a fast clean verdict to retry.
            master->abort_migration();
            return "participant faulted during copy1 -> aborted (source intact, never fenced)";
        }
        master->lock_range();          // Phase 2: master freezes the source's range
        if (!master->drain_source()) { // wait out writes that began pre-fence
            master->abort_migration();
            return "write drain timed out -> aborted (source intact, unfrozen)";
        }
        Log_info("MigrationAdmin: '%s' %d->%d phase lock+drain took %ld ms",
                 req.table_name.c_str(), req.src, req.dst, phase_ms());
        master->background_copy();     // catch-up: the range is now QUIESCENT
        Log_info("MigrationAdmin: '%s' %d->%d phase copy2 took %ld ms",
                 req.table_name.c_str(), req.src, req.dst, phase_ms());
        master->final_sync();          // Phase 3: checksum gate -> dest prepared
        Log_info("MigrationAdmin: '%s' %d->%d phase final_sync took %ld ms",
                 req.table_name.c_str(), req.src, req.dst, phase_ms());
        if (!master->both_prepared()) {
            // Distinguish a dead/unreachable participant (its reads degenerate
            // to empty/0, which the master's faulted() gate refuses to treat as
            // a checksum match) from a genuine copy divergence.
            const bool faulted = master->participants_faulted();
            const size_t src_n = master->shard_range_count(
                static_cast<uint32_t>(req.src), publish_lo, publish_hi);
            const size_t dst_n = master->shard_range_count(
                static_cast<uint32_t>(req.dst), publish_lo, publish_hi);
            // The checksum PAIR, read pre-abort while the fence still holds
            // the range still: this is the ground truth a divergence abort
            // needs (the counts above go through the remote count path, which
            // is known to read 0 for wh-specs).
            uint64_t src_ck = 0, dst_ck = 0;
            {
                ShardData* s = ParticipantFor(static_cast<uint32_t>(req.src),
                                              req.table_name);
                ShardData* d = ParticipantFor(static_cast<uint32_t>(req.dst),
                                              req.table_name);
                if (s != nullptr) src_ck = s->checksum(publish_lo, publish_hi);
                if (d != nullptr) dst_ck = d->checksum(publish_lo, publish_hi);
                Log_warn("MigrationAdmin: final_sync diverged for '%s': src rows=%zu "
                         "dst rows=%zu src_ck=%llu dst_ck=%llu faulted=%d",
                         req.table_name.c_str(), src_n, dst_n,
                         static_cast<unsigned long long>(src_ck),
                         static_cast<unsigned long long>(dst_ck), faulted ? 1 : 0);
                // Row-level diff on a REAL divergence (both folds well-formed
                // but different): still pre-abort, so both sides are fenced
                // and still. Names the first few mismatching rows -- the
                // guilty value shape identifies the copy-fidelity bug.
                if (src_ck != 0 && dst_ck != 0 && src_ck != dst_ck &&
                    s != nullptr && d != nullptr) {
                    auto hex = [](const std::string& b, size_t off, size_t n) {
                        static const char* k = "0123456789abcdef";
                        std::string h;
                        for (size_t x = off; x < b.size() && x < off + n; x++) {
                            unsigned char c = static_cast<unsigned char>(b[x]);
                            h += k[c >> 4]; h += k[c & 15];
                        }
                        return h;
                    };
                    // Scan with the EXPLICIT whole-keyspace range: the remote
                    // scan path chunks CLIENT-side from the caller's lo, and
                    // wh-spec publish keys (warehouse_route_key space) never
                    // match physical rows -- the same artifact that makes the
                    // counts above read 0 for the remote side.
                    const std::string diff_lo;
                    const std::string diff_hi(16, '\xff');
                    auto sv = s->scan_range(diff_lo, diff_hi);
                    auto dv = d->scan_range(diff_lo, diff_hi);
                    Log_warn("MigrationAdmin: diff scan src=%zu dst=%zu rows",
                             sv.size(), dv.size());
                    size_t i = 0, j = 0;
                    int logged = 0;
                    while ((i < sv.size() || j < dv.size()) && logged < 3) {
                        if (j >= dv.size() ||
                            (i < sv.size() && sv[i].first < dv[j].first)) {
                            Log_warn("  diff: key only on SRC k[%zu]=%s",
                                     sv[i].first.size(),
                                     hex(sv[i].first, 0, 24).c_str());
                            i++; logged++;
                        } else if (i >= sv.size() || dv[j].first < sv[i].first) {
                            Log_warn("  diff: key only on DST k[%zu]=%s",
                                     dv[j].first.size(),
                                     hex(dv[j].first, 0, 24).c_str());
                            j++; logged++;
                        } else {
                            if (sv[i].second != dv[j].second) {
                                size_t o = 0;
                                const std::string& a = sv[i].second;
                                const std::string& b = dv[j].second;
                                while (o < a.size() && o < b.size() && a[o] == b[o]) o++;
                                Log_warn("  diff: k[%zu]=%s src_vlen=%zu dst_vlen=%zu "
                                         "first_diff@%zu src[..]=%s dst[..]=%s",
                                         sv[i].first.size(),
                                         hex(sv[i].first, 0, 24).c_str(),
                                         a.size(), b.size(), o,
                                         hex(a, o, 16).c_str(), hex(b, o, 16).c_str());
                                logged++;
                            }
                            i++; j++;
                        }
                    }
                }
            }
            master->abort_migration();
            return faulted
                ? "participant unreachable -> aborted (source intact and "
                  "unfrozen; retry when the shard is back)"
                : "checksum verify failed -> aborted (source intact, unfrozen)";
        }
        if (!master->commit_migration())  // Phase 4: shed + publish cutover
            return "commit_migration failed";
        *moved = static_cast<rrr::i64>(master->shard_range_count(
            static_cast<uint32_t>(req.dst), publish_lo, publish_hi));
        // Post-commit shed verification: the cutover is already published (dest
        // owns the range) -- residual source rows are unrouted garbage, surfaced
        // here so an operator can re-drop them (e.g. after a source that died
        // between its prepare vote and the DropRange).
        const size_t src_left = master->shard_range_count(
            static_cast<uint32_t>(req.src), publish_lo, publish_hi);
        if (src_left > 0) {
            Log_warn("MigrationAdmin: committed, but the source still holds %zu "
                     "rows in [%s,%s) (DropRange failed?); unrouted until re-dropped",
                     src_left, publish_lo.c_str(), publish_hi.c_str());
        }
        return std::string();
        } catch (mako::oi_scan_wedged&) {
            master->abort_migration();
            return "copy scan wedged on a leaked row lock -> aborted (fences rolled back)";
        } catch (abstract_db::abstract_abort_exception&) {
            master->abort_migration();
            return "copy hit a migration fence -> aborted (fences rolled back)";
        }
    }

    // Attach shard `sid`'s data plane FOR `table` to the master (participants
    // are table-bound; attach_shard overwrites, so each migration binds the
    // handles for ITS table): this process's catalog entry for the local shard,
    // a table-bound RemoteShardData over the peer's ShardDataService otherwise.
    // Proxies are cached per shard, handles per (shard, table); all leaked
    // (process-lifetime). Returns "" or the failure detail.
    // The participant handle for (shard, table) -- the local catalog entry or
    // the cached remote handle -- for diagnostics that need direct reads
    // (e.g. the divergence checksum pair). nullptr if never attached.
    ShardData* ParticipantFor(uint32_t sid, const std::string& table) {
        auto& bench = BenchmarkConfig::getInstance();
        if (sid == static_cast<uint32_t>(bench.getShardIndex())) {
            return g_data_catalog != nullptr ? g_data_catalog->get_or_create(table)
                                             : nullptr;
        }
        auto hit = remotes_.find(std::to_string(sid) + "/" + table);
        return hit != remotes_.end() ? hit->second : nullptr;
    }

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
        const std::string addr = data_plane_addr_of(sid);
        const std::string hkey = std::to_string(sid) + "/" + table;
        auto hit = remotes_.find(hkey);
        if (hit != remotes_.end()) {
            if (!hit->second->faulted()) {
                master->attach_shard(sid, hit->second);
                return std::string();
            }
            // A latched fault poisons every later migration through this handle:
            // evict it AND the shard's connection (the fault usually means the
            // peer/connection died), then rebuild fresh below -- this is what
            // makes retry-after-failure work for REMOTE participants.
            remotes_.erase(hit);
            proxies_.erase(sid);
            Log_info("MigrationAdmin: evicted faulted handle for shard %u table '%s'",
                     sid, table.c_str());
        }
        ShardDataServiceProxy* proxy = nullptr;
        auto pit = proxies_.find(sid);
        if (pit != proxies_.end()) {
            proxy = pit->second;
        } else {
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
        // Table- and address-bound: the addr lets this participant act as a pull
        // SOURCE, and lets the coordinator delegate copies to it as a pull
        // DESTINATION (one PullRange control RPC; rows go source -> dest direct).
        auto* remote = new RemoteShardData(proxy, table, addr);   // leaked
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
    // Each table self-identifies with this shard's data-plane address so remote
    // migration destinations can pull ranges directly from here.
    g_data_catalog = mako::make_engine_shard_catalog(
        db, data_plane_addr_of(static_cast<uint32_t>(shard)), shard);
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
    if (g_data_server->start(bind_addr.c_str()) != 0) {
        Log_warn("ShardDataPlane: failed to bind %s", bind_addr.c_str());
        return;
    }
    Log_info("ShardDataPlane: shard %d data plane listening on %s",
             shard, bind_addr.c_str());
    // The admin control service on its OWN server + poll (see
    // kMigrationAdminPortDelta): a blocking Migrate must never occupy the
    // data-plane worker that serves this shard's ScanRange/PullRange.
    if (shard == 0) {
        std::string admin_addr = "0.0.0.0:" +
            std::to_string(atoi(me.port.c_str()) + kMigrationAdminPortDelta);
        g_admin_srv_poll = rusty::Some(rrr::PollThread::create());
        g_admin_server = new rrr::Server(
            rusty::Some(g_admin_srv_poll.as_ref().unwrap().clone()));
        g_admin_server->reg_service(rusty::make_box<MigrationAdminImpl>());
        if (g_admin_server->start(admin_addr.c_str()) != 0) {
            Log_warn("ShardDataPlane: MigrationAdmin failed to bind %s",
                     admin_addr.c_str());
            return;
        }
        Log_info("ShardDataPlane: MigrationAdmin listening on %s", admin_addr.c_str());
    }
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

namespace mako {

// The workload side of the mixed routing strategy: the partition table guards
// routing, and TPC-C fills it with sharding-by-warehouse. Called from tpcc.cc
// init_tables (which runs after BootstrapClusterConfig on the leader); only
// the shard-0 leader owns the authoritative WRITABLE ConfigManager -- on every
// other process g_cfg_cm wraps the read-only remote store, so this no-ops and
// they learn the seeded partitions through their ConfigWatcher.
// @unsafe - writes through the master's ConfigManager
bool tpcc_seed_warehouse_partitions_if_master(int num_warehouses_total,
                                              int num_shards) {
    if (janus::g_shard_master.is_none()) return false;   // not the master process
    if (janus::g_cfg_cm.is_none()) return false;
    janus::ConfigManager* cm = janus::g_cfg_cm.as_ref().unwrap().get();
    if (cm->get_sharding_mode() != "map") return false;  // partition routing not chosen
    // Every warehouse-partitioned TPC-C table. item is deliberately absent:
    // read-only + replicated per shard, its routing must stay local.
    static const char* kTables[] = {
        "customer", "customer_name_idx", "district", "history", "new_order",
        "oorder", "oorder_c_id_idx", "order_line", "stock", "stock_data",
        "warehouse"};
    bool ok = true;
    for (const char* t : kTables) {
        if (!mako::seed_warehouse_partitions(cm, std::string(t),
                                             num_warehouses_total, num_shards)) {
            Log_warn("tpcc partition seeding failed for table %s", t);
            ok = false;
        }
    }
    if (ok) {
        Log_info("tpcc partition seeding: %d warehouses across %d shards "
                 "(11 warehouse-partitioned tables now governed)",
                 num_warehouses_total, num_shards);
    }
    return ok;
}

}  // namespace mako
