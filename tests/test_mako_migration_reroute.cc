// Live online-migration REROUTE over the real non-txn client->server RPC path,
// with TWO real shard servers in one process.
//
// This is the end-to-end assembly the individual gating tests could not show on
// their own:
//   - test_mako_nontxn_distributed proves the freeze + the nontxn RPC path over
//     ONE server.
//   - shard_router_test.MigrationReroutesComputeShardForKey proves the routing
//     entry reroutes after a migration (in-process, no RPC).
//   - shard_migration_mbta_test proves the migration moves data on real mbta.
// Here all three meet: a client seeds keys that route to shard 0's server, the
// ShardMaster physically moves a key range onto shard 1's INDEPENDENT store and
// republishes the process-global ClusterConfig, and the SAME client's later
// nontxnGet for a migrated key is served by shard 1 (rerouted) returning the
// value that was written to shard 0 (no loss) -- while keys outside the moved
// range still route to shard 0.
//
// Two co-located servers work because a request lands on a server by PORT:
// FastTransport binds shard s at base(s)+id and the client sends to
// base(dstShard)+id (rrr_rpc_backend.cc), with id = warehouses+5+0 = 6 for both.
// So shard 0's server is at base(0)+6, shard 1's at base(1)+6, and the client
// (base(0)+par_id=+0) collides with neither. Each mbta_index_build does a fresh
// `new mbta_table`, so the two servers' stores are physically independent even
// under the same table id -- the migration really moves rows between them.

#include <stdlib.h>
#include <unistd.h>

#include "benchmarks/bench.h"
#include "storage/mbta_wrapper.hh"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/shardClient.h"
#include "lib/table_registry.h"          // mako::get_table_registry() -> table name for id
#include "ordered_index_shard_data.h"    // janus::OrderedIndexShardData over real mbta
#include "sto/Transaction.hh"
#include "sto/sync_util.hh"

#include <gtest/gtest.h>

import std;
import cluster;   // ShardMaster / ConfigManager / InMemoryKvStore / get_cluster_config

namespace {

constexpr int  kClientShard   = 0;     // the client's own shard identity
constexpr int  kParId         = 0;
constexpr int  kNumWarehouses = 1;
constexpr int  kTableId       = 201;   // the logical non-txn table the client addresses

transport::Configuration* g_config = nullptr;
mbta_wrapper*             g_db     = nullptr;

// One real server shard: its own transport (bound to base(shard)+6) and its own
// INDEPENDENT is_remote=false store, registered under kTableId.
struct ServerShard {
    int                  shard_idx = -1;
    FastTransport*       transport = nullptr;
    mbta_ordered_index*  store     = nullptr;
};
ServerShard g_srv[2];

// The authoritative config + routing cache the migration republishes. Static so
// the ShardMaster (which borrows them) and the process-global routing entry all
// see the SAME objects; the reload on commit is what makes compute_shard_for_key
// -- and therefore the client's next RPC -- reroute.
janus::InMemoryKvStore* g_kv = nullptr;
janus::ConfigManager*   g_cm = nullptr;
std::string             g_table_name;   // what compute_shard_for_key derives for kTableId

std::string config_path() {
    const char* candidates[] = {
        "./src/mako/config/local-shards2-warehouses1.yml",
        "../src/mako/config/local-shards2-warehouses1.yml",
    };
    for (const char* c : candidates) {
        if (access(c, R_OK) == 0) return c;
    }
    ADD_FAILURE() << "config yml not found from cwd";
    return candidates[0];
}

// Bind the fake (UDP) transport to shard `si`'s URI at base(si)+6 and run its
// event loop. Mirrors test_mako_nontxn_distributed's rpc_server_thread, but
// parameterized by shard so we can stand up BOTH shard servers.
void rpc_server_thread(int si) {
    std::string local_uri =
        g_config->shard(si, mako::convertCluster("localhost")).host;
    int id = kNumWarehouses + 5 + 0;   // = 6, same offset on both shards
    g_srv[si].transport = new FastTransport(g_config->configFile,
                                            local_uri,
                                            "localhost",
                                            1, 17,
                                            0,   // physPort
                                            0,   // numa node
                                            si,  // shard -> base port
                                            id);
    std::unordered_map<uint16_t, mako::HelperQueue*> queues;
    std::unordered_map<uint16_t, mako::HelperQueue*> queues_response;
    queues[0] = new mako::HelperQueue(0, true);
    queues_response[0] = new mako::HelperQueue(0, false);
    g_srv[si].transport->SetHelperQueues(queues);
    g_srv[si].transport->SetHelperQueuesResponse(queues_response);
    g_srv[si].transport->Run();
}

// The worker servicing non-txn RPCs for shard `si` against its own store.
void helper_server_thread(int si) {
    scoped_db_thread_ctx ctx(g_db, true, 1);   // source=1 => helper_server
    TThread::set_mode(1);
    TThread::enable_multiverison();
    TThread::set_shard_index(si);
    TThread::set_pid(kParId);
    TThread::set_nshards(g_config->nshards);
    auto* ss = new mako::ShardServer(g_config->configFile,
                                     kClientShard,   // clientShardIndex (cosmetic here)
                                     si,             // serverShardIndex (cosmetic here)
                                     kParId);
    std::map<int, abstract_ordered_index*> tables;
    tables[kTableId] = g_srv[si].store;
    ss->Register(g_db,
                 g_srv[si].transport->GetHelperQueue(0),
                 g_srv[si].transport->GetHelperQueueResponse(0),
                 tables);
    ss->Run();   // event driven
}

// Results the migration thread hands back to the main thread. Direct store reads
// (storeA/storeB.get) need an mbta-registered thread, so they run WITH the
// migration and are asserted after join.
struct MigrationProbe {
    bool   begin_ok    = false;
    bool   prepared_ok = false;
    bool   commit_ok   = false;
    // After commit: does each shard's store hold a MID key (in the moved range)?
    bool   src_has_mid = true;    // expect false: shard 0 shed it
    bool   dst_has_mid = false;   // expect true : shard 1 received it
    std::string dst_mid_val;      // value read straight from shard 1's store
    // A LOW key (outside the moved range) must stay on shard 0.
    bool   src_has_low = false;   // expect true
    bool   dst_has_low = true;    // expect false
    size_t src_range_after = 999; // expect 0  (shard 0 shed [lo,hi))
    size_t dst_range_after = 0;   // expect 10 (shard 1 owns [lo,hi))
};

class MakoMigrationReroute : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_config = new transport::Configuration(config_path());
        BenchmarkConfig::getInstance().setConfig(g_config);

        // Term-0 callbacks (as in test_mako_nontxn_distributed): unregistered
        // sync-util std::functions would throw under FAIL_NEW_VERSION.
        register_sync_util_sc([]() { return 0; });
        register_sync_util_ss([]() { return 0; });

        g_db = new mbta_wrapper;
        {
            scoped_db_thread_ctx ctx(g_db, /*loader=*/true);   // register main thread
        }

        // Two INDEPENDENT server stores under the same logical table id: fresh
        // `new mbta_table` each, so the migration moves rows between real stores.
        g_srv[0].shard_idx = 0;
        g_srv[1].shard_idx = 1;
        g_srv[0].store = mbta_index_build("nontxn_reroute", kTableId, /*is_remote=*/false);
        g_srv[1].store = mbta_index_build("nontxn_reroute", kTableId, /*is_remote=*/false);

        // Stand up both shard servers (transport first, then its worker).
        for (int si = 0; si < 2; si++) {
            std::thread(rpc_server_thread, si).detach();
        }
        sleep(2);
        for (int si = 0; si < 2; si++) {
            std::thread(helper_server_thread, si).detach();
        }
        sleep(1);

        // Client on this thread; its SendToShard(dstShard) reaches either server.
        TThread::sclient = new mako::ShardClient(g_config->configFile,
                                                 "localhost",
                                                 kClientShard,
                                                 kParId);

        // Map-mode routing plane: whole keyspace starts on shard 0. Seed the
        // partition for EXACTLY the table name compute_shard_for_key derives for
        // kTableId, so the client's routing and the migration agree.
        auto name_opt = mako::get_table_registry().get_table_name(kTableId);
        g_table_name = name_opt.is_some() ? name_opt.as_ref().unwrap() : std::string();

        g_kv = new janus::InMemoryKvStore();
        g_cm = new janus::ConfigManager(g_kv);
        ASSERT_TRUE(g_cm->add_shard(0, {"s0"}));
        ASSERT_TRUE(g_cm->add_shard(1, {"s1"}));
        ASSERT_TRUE(g_cm->set_sharding_mode("map"));
        ASSERT_TRUE(g_cm->seed_partition(g_table_name, 0));
        ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(g_cm));
    }

    static std::string key(int i) {
        return std::string("k") + (i < 10 ? "0" : "") + std::to_string(i);
    }
    static std::string val(int i) { return "v" + std::to_string(i); }
};

// ---------------------------------------------------------------------------
// Seed on shard 0 -> migrate [k10,k20) 0->1 -> the client's nontxnGet for a
// migrated key is served by shard 1 (rerouted, no loss); keys outside stay on 0.
// ---------------------------------------------------------------------------
TEST_F(MakoMigrationReroute, MigratedRangeReroutesToNewOwnerShard) {
    // --- Seed 30 keys via the REAL client RPC path. Map mode -> all route to
    //     shard 0 -> land in shard 0's store. ---
    for (int i = 0; i < 30; i++) {
        bool op = false;
        ASSERT_EQ(TThread::sclient->nontxnPut(kTableId, key(i), val(i), &op),
                  static_cast<int>(mako::ErrorCode::SUCCESS)) << key(i);
    }

    // Everything routes to shard 0 up front (nothing migrated yet).
    EXPECT_EQ(0, mako::compute_shard_for_key(kTableId, key(15)));
    EXPECT_EQ(0, mako::compute_shard_for_key(kTableId, key(5)));
    {
        std::string out;
        ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, key(15), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, val(15));   // served by shard 0
    }

    // --- Migrate [k10,k20) from shard 0 to shard 1 on a dedicated mbta thread
    //     (OrderedIndexShardData / range scans need an engine-registered thread).
    //     This drives the SAME ShardMaster the cluster uses; commit republishes
    //     g_cm and reloads the process-global ClusterConfig. ---
    const std::string lo = key(10), hi = key(20);
    MigrationProbe probe;
    std::thread mig([&] {
        scoped_db_thread_ctx ctx(g_db, /*loader=*/true);   // engine context for scan/put

        janus::OrderedIndexShardData sd0(g_srv[0].store);
        janus::OrderedIndexShardData sd1(g_srv[1].store);
        janus::ShardMaster master =
            janus::ShardMaster::new_(g_cm, &janus::get_cluster_config());
        master.attach_shard(0, &sd0);
        master.attach_shard(1, &sd1);

        probe.begin_ok = master.begin_migration(0, 1, g_table_name, lo, hi);
        master.background_copy();      // Phase 1: copy under the source still serving
        master.lock_range();           // Phase 2: freeze the range
        master.final_sync();           // Phase 3: delta replay + checksum gate
        probe.prepared_ok = master.both_prepared();
        probe.commit_ok = master.commit_migration();   // Phase 4: source sheds + republish

        // Direct store reads (this thread is engine-registered).
        std::string mv;
        probe.dst_has_mid = g_srv[1].store->get(lcdf::Str(key(15)), mv, std::string::npos);
        probe.dst_mid_val = mv;
        std::string tmp;
        probe.src_has_mid = g_srv[0].store->get(lcdf::Str(key(15)), tmp, std::string::npos);
        probe.src_has_low = g_srv[0].store->get(lcdf::Str(key(5)),  tmp, std::string::npos);
        probe.dst_has_low = g_srv[1].store->get(lcdf::Str(key(5)),  tmp, std::string::npos);
        probe.src_range_after = master.shard_range_count(0, lo, hi);
        probe.dst_range_after = master.shard_range_count(1, lo, hi);
    });
    mig.join();

    // --- The migration itself: prepared, committed, data physically moved. ---
    EXPECT_TRUE(probe.begin_ok);
    EXPECT_TRUE(probe.prepared_ok);
    EXPECT_TRUE(probe.commit_ok);
    EXPECT_FALSE(probe.src_has_mid) << "shard 0 must shed the migrated key";
    EXPECT_TRUE(probe.dst_has_mid)  << "shard 1 must hold the migrated key";
    EXPECT_EQ(probe.dst_mid_val, val(15));
    EXPECT_TRUE(probe.src_has_low)  << "an unmigrated key stays on shard 0";
    EXPECT_FALSE(probe.dst_has_low) << "an unmigrated key must not be on shard 1";
    EXPECT_EQ(probe.src_range_after, 0u);
    EXPECT_EQ(probe.dst_range_after, 10u);

    // --- The routing entry reroutes the moved range to shard 1; neighbors stay. ---
    EXPECT_EQ(1, mako::compute_shard_for_key(kTableId, key(15)));   // inside -> shard 1
    EXPECT_EQ(0, mako::compute_shard_for_key(kTableId, key(5)));    // below  -> shard 0
    EXPECT_EQ(0, mako::compute_shard_for_key(kTableId, key(25)));   // above  -> shard 0

    // --- The money shot: the SAME client's nontxnGet for a migrated key is now
    //     served by shard 1. Shard 0 shed k15, so a SUCCESS here can ONLY have
    //     come from shard 1 -- the client rerouted, and the value survived. ---
    {
        std::string out;
        ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, key(15), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS))
            << "migrated key must be served by its new owner (shard 1)";
        EXPECT_EQ(out, val(15)) << "no lost write across the migration";
    }
    // A key that never moved is still served (by shard 0).
    {
        std::string out;
        ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, key(5), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, val(5));
    }
    {
        std::string out;
        ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, key(25), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, val(25));
    }
}

// ---------------------------------------------------------------------------
// Freeze-under-migration: while a range is frozen for the migration window, a
// client write to that range is rejected (SERVER_BUSY) and does NOT land, reads
// and out-of-range writes stay available, and after the cutover + unfreeze the
// client's (retried) write lands on the NEW owner with the pre-migration value
// preserved. This is the online-migration freeze contract end-to-end over the
// real 2-server RPC path.
//
// Single-threaded and deterministic: the freeze is HELD across the whole
// migration (the mbta ops run on their own engine thread, joined before the
// unfreeze), so every observation has one correct outcome regardless of timing.
// A real client's put() wrapper RETRIES on SERVER_BUSY -- exactly the freeze
// contract -- so the "reject now, land on the destination after cutover" here is
// what that retry does, made observable step by step. Disjoint m* key range so
// this is independent of the reroute test in this same fixture.
// ---------------------------------------------------------------------------
TEST_F(MakoMigrationReroute, FrozenRangeRejectsRangeWritesDuringMigrationThenReroutes) {
    auto mkey = [](int i) {
        return std::string("m") + (i < 10 ? "0" : "") + std::to_string(i);
    };
    auto mval = [](int i) { return "w" + std::to_string(i); };
    janus::get_migration_guard().clear();

    // Seed m00..m29 on shard 0 (map mode -> the whole partition is still on 0).
    for (int i = 0; i < 30; i++) {
        bool op = false;
        ASSERT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(i), mval(i), &op),
                  static_cast<int>(mako::ErrorCode::SUCCESS)) << mkey(i);
    }

    const std::string lo = mkey(10), hi = mkey(20);

    // FREEZE [m10,m20) -- stands in for the coordinator's FreezeRange RPC on the
    // SOURCE. lock_range() only flips the master's internal 2PC flag; the freeze
    // that the server's RunNontxnOp actually enforces is the process-global guard,
    // so the coordinator freezes it explicitly. The range stops taking writes.
    janus::get_migration_guard().freeze(std::string(), lo, hi);
    bool op = false;

    // DURING the freeze:
    //  (a) a write INSIDE the range is rejected and does NOT land,
    EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(15), "SHOULD_NOT_LAND", &op),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY));
    //  (b) reads of the range stay available (gets are never frozen),
    {
        std::string out;
        EXPECT_EQ(TThread::sclient->nontxnGet(kTableId, mkey(15), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, mval(15)) << "the rejected write must not have landed";
    }
    //  (c) writes OUTSIDE the range stay available (only the moving range freezes).
    EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(5), "live-below", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(25), "live-above", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));

    // Run the migration while the range stays frozen (stable range -> the single
    // copy captures everything; the empty delta replays cleanly).
    MigrationProbe probe;
    std::thread mig([&] {
        scoped_db_thread_ctx ctx(g_db, /*loader=*/true);
        janus::OrderedIndexShardData sd0(g_srv[0].store);
        janus::OrderedIndexShardData sd1(g_srv[1].store);
        janus::ShardMaster master =
            janus::ShardMaster::new_(g_cm, &janus::get_cluster_config());
        master.attach_shard(0, &sd0);
        master.attach_shard(1, &sd1);
        probe.begin_ok = master.begin_migration(0, 1, g_table_name, lo, hi);
        master.background_copy();
        master.lock_range();
        master.final_sync();
        probe.prepared_ok = master.both_prepared();
        probe.commit_ok = master.commit_migration();
        std::string mv;
        probe.dst_has_mid = g_srv[1].store->get(lcdf::Str(mkey(15)), mv, std::string::npos);
        probe.dst_mid_val = mv;
        std::string tmp;
        probe.src_has_mid = g_srv[0].store->get(lcdf::Str(mkey(15)), tmp, std::string::npos);
        probe.src_range_after = master.shard_range_count(0, lo, hi);
        probe.dst_range_after = master.shard_range_count(1, lo, hi);
    });
    mig.join();

    // Migration committed; the range physically moved with its PRE-migration
    // values (the rejected write never landed -> m15 is still the seeded value).
    EXPECT_TRUE(probe.begin_ok);
    EXPECT_TRUE(probe.prepared_ok);
    EXPECT_TRUE(probe.commit_ok);
    EXPECT_FALSE(probe.src_has_mid);
    EXPECT_TRUE(probe.dst_has_mid);
    EXPECT_EQ(probe.dst_mid_val, mval(15)) << "no lost write: the seeded value migrated intact";
    EXPECT_EQ(probe.src_range_after, 0u);
    EXPECT_EQ(probe.dst_range_after, 10u);

    // Cutover published -> the moved range routes to shard 1.
    EXPECT_EQ(1, mako::compute_shard_for_key(kTableId, mkey(15)));
    EXPECT_EQ(0, mako::compute_shard_for_key(kTableId, mkey(5)));

    // The guard is process-global (""), so the moved range is STILL frozen on its
    // new owner (shard 1) until the coordinator lifts it: a write is still rejected,
    // but a READ is served by shard 1 with the migrated value.
    EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(15), "still-frozen", &op),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY));
    {
        std::string out;
        EXPECT_EQ(TThread::sclient->nontxnGet(kTableId, mkey(15), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, mval(15)) << "migrated value is served by the new owner while frozen";
    }

    // UNFREEZE -> shard 1 serves the range. The client's retried write now lands
    // on the NEW owner (this is what the put() wrapper's SERVER_BUSY retry does).
    janus::get_migration_guard().unfreeze(std::string(), lo, hi);
    EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, mkey(15), "after-cutover", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    {
        std::string out;
        ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, mkey(15), out),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
        EXPECT_EQ(out, "after-cutover") << "the retried write landed on shard 1";
    }

    janus::get_migration_guard().clear();
}

}  // namespace
