// Cross-process online-migration demo endpoint (see migration_demo_endpoint.h).
//
// The rrr + rcc_rpc + cluster-module side of the demo, in its OWN TU (no Masstree,
// no gtest) so it compiles fast -- the same profile as src/mako/cluster_bootstrap.cc
// and tests/shard_data_rpc_harness.cc. dbtest.cc (Masstree-heavy) only ever sees
// the thin header.

#include "migration_demo_endpoint.h"

#include <stdio.h>
#include <unistd.h>   // sleep
#include <map>
#include <string>
#include <vector>

import cluster;   // ShardData / KvPair / ShardMaster / ConfigManager / InMemoryKvStore / ClusterConfig

#include "shard_data_service.h"   // ShardDataServiceImpl / RemoteShardData / ShardDataServiceProxy (rcc_rpc.h)
#include "rrr/rrr.hpp"            // rrr::Server / Client / PollThread, Log_*

namespace mako {
namespace {

// In-memory ShardData backend: a sorted std::map, so scan_range returns ascending
// pairs. Same shape as tests/shard_migration_rpc_test.cc's MapShardData. Safe to
// touch from the rrr poll thread (no mbta thread context required). The migration
// range ops (checksum / verify_range / drop_range / range_count) are inherited
// from ShardData, defined over these primitives.
// @unsafe - plain std::map behind the ShardData port; single demo thread + poll thread.
class MapShardData : public janus::ShardData {
public:
    void put(const std::string& k, const std::string& v) override { m_[k] = v; }
    bool get(const std::string& k, std::string& out) override {
        auto it = m_.find(k);
        if (it == m_.end()) return false;
        out = it->second;
        return true;
    }
    void remove(const std::string& k) override { m_.erase(k); }
    std::vector<KvPair> scan_range(const std::string& lo,
                                          const std::string& hi) override {
        std::vector<KvPair> o;
        for (auto& kv : m_)
            if (kv.first >= lo && kv.first < hi) o.emplace_back(kv.first, kv.second);
        return o;
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                                  const std::string& hi,
                                                  size_t limit) override {
        std::vector<KvPair> o;
        for (auto& kv : m_) {
            if (kv.first >= lo && kv.first < hi) {
                o.emplace_back(kv.first, kv.second);
                if (o.size() >= limit) break;
            }
        }
        return o;
    }
private:
    std::map<std::string, std::string> m_;
};

std::string dkey(int i) {
    char b[16];
    snprintf(b, sizeof b, "d%02d", i);
    return std::string(b);
}

}  // namespace

// @unsafe - rrr framework wiring (raw Server, leaked singletons); same shape as
// cluster_bootstrap.cc's ConfigKvService bring-up.
int xproc_migration_serve(const std::string& bind_addr, int count) {
    auto* src = new MapShardData();
    for (int i = 0; i < count; i++)
        src->put(dkey(i), std::string("v") + std::to_string(i));

    auto* spoll  = new rusty::Arc<rrr::PollThread>(rrr::PollThread::create());
    auto* server = new rrr::Server(rusty::Some(spoll->clone()));
    server->reg_service(rusty::make_box<janus::ShardDataServiceImpl>(src));
    int rc = server->start(bind_addr.c_str());
    if (rc == 0)
        Log_info("XPROC MIGRATION: source serving %d demo keys on %s", count, bind_addr.c_str());
    else
        Log_warn("XPROC MIGRATION: source failed to bind %s (rc=%d)", bind_addr.c_str(), rc);
    return rc;
}

// @unsafe - rrr client connect + the cluster ShardMaster driving a real migration.
long xproc_migration_run(const std::string& src_addr, const std::string& lo,
                         const std::string& hi, int connect_retries) {
    auto* cpoll  = new rusty::Arc<rrr::PollThread>(rrr::PollThread::create());
    auto* client = new rusty::Arc<rrr::Client>(rrr::Client::create(cpoll->clone()));
    int attempt = 0;
    while ((*client)->connect(src_addr.c_str(), false) != 0) {
        if (++attempt > connect_retries) {
            Log_warn("XPROC MIGRATION: dest could not connect to source %s after %d tries",
                     src_addr.c_str(), connect_retries);
            return -1;
        }
        sleep(1);
    }
    auto* proxy      = new janus::ShardDataServiceProxy(const_cast<rrr::Client*>(client->get()));
    auto* remote_src = new janus::RemoteShardData(proxy);   // source lives in the OTHER process
    auto* local_dst  = new MapShardData();                  // dest is here

    // A self-contained coordinator over an isolated in-memory config (map mode,
    // whole "" keyspace seeded to the source), so the demo never perturbs the
    // process's real routing. shard 0 = source (remote), shard 1 = dest (local).
    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    cm.add_shard(0, {"src"});
    cm.add_shard(1, {"dst"});
    cm.set_sharding_mode("map");
    cm.seed_partition("", 0);
    janus::ClusterConfig cfg = janus::ClusterConfig::new_();
    cfg.load_from_config_manager(&cm);

    janus::ShardMaster master = janus::ShardMaster::new_(&cm, &cfg);
    master.attach_shard(0, remote_src);
    master.attach_shard(1, local_dst);

    if (!master.begin_migration(0, 1, "", lo, hi)) {
        Log_warn("XPROC MIGRATION: begin_migration failed");
        return -1;
    }
    master.background_copy();   // ScanRange RPCs pull [lo,hi) from the remote source
    master.lock_range();
    master.final_sync();        // Checksum RPC (source) vs local verify_range (2PC prepare)
    if (!master.both_prepared()) {
        master.abort_migration();
        Log_warn("XPROC MIGRATION: checksum mismatch -> aborted");
        return -1;
    }
    if (!master.commit_migration()) {   // DropRange RPC sheds the range on the remote source
        Log_warn("XPROC MIGRATION: commit_migration failed");
        return -1;
    }

    long moved    = static_cast<long>(master.shard_range_count(1, lo, hi));  // now local
    long src_left = static_cast<long>(master.shard_range_count(0, lo, hi));  // remote, via RPC scan
    Log_info("XPROC MIGRATION: OK moved %ld rows [%s,%s) from the remote source into the "
             "local dest over rrr (remote source now holds %ld)",
             moved, lo.c_str(), hi.c_str(), src_left);
    return moved;
}

}  // namespace mako
