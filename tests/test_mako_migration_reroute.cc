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
    // One helper queue per requesting client identity (server_id = the
    // client's global warehouse id): 0 = the main test client (par_id 0),
    // 1 = the racing writer (par_id 1). Mirrors production's one helper
    // per client warehouse.
    for (uint16_t q = 0; q <= 1; q++) {
        queues[q] = new mako::HelperQueue(q, true);
        queues_response[q] = new mako::HelperQueue(q, false);
    }
    g_srv[si].transport->SetHelperQueues(queues);
    g_srv[si].transport->SetHelperQueuesResponse(queues_response);
    g_srv[si].transport->Run();
}

// The worker servicing non-txn RPCs for shard `si` against its own store,
// consuming helper queue `qid` (one worker per client identity).
void helper_server_thread(int si, uint16_t qid) {
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
                 g_srv[si].transport->GetHelperQueue(qid),
                 g_srv[si].transport->GetHelperQueueResponse(qid),
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
            std::thread(helper_server_thread, si, static_cast<uint16_t>(0)).detach();
            std::thread(helper_server_thread, si, static_cast<uint16_t>(1)).detach();
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

    // COMMIT transfers ownership: the master unfences the DESTINATION (clearing
    // any stale fence from a previous ownership of this range). In this
    // single-process bed the guard is SHARED and the manual + lock entries carry
    // the identical ("", lo, hi) triple, so the destination's unfence clears the
    // source's too -- a bed artifact; in production each process has its own
    // guard and the source keeps its fence (the coordinator-level contract is
    // pinned by shard_master_test.AbortUnfreezesSourceCommitLeavesFence). The
    // client's retried write therefore lands on the NEW owner right away here.
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

// ---------------------------------------------------------------------------
// THE RACE TEST (TxnMig T2): a second client hammers the migrating range
// through the real server path WHILE the migration freezes mid-stream, with the
// full corrected sequence -- copy, lock (staging fence), DRAIN (Silo epoch
// wait), catch-up copy, checksum, commit. The safety property, timing-free:
//
//     every ACKNOWLEDGED write is present with its value on the final owner.
//
// Before the staging fence + drain, a writer could pass the freeze check, have
// its one-op txn commit after the checksum scans, and lose an acked write to
// drop_range. The fence makes post-install writes abort retryably
// (SERVER_BUSY); the drain waits out pre-install in-flight ones before the
// catch-up copy; acks are therefore either copied or landed post-cutover on
// the destination.
//
// The racing client runs on its own thread with its own ShardClient (sclient
// is per-thread; par_id 1 binds port base+1) and pins the_num_rpc_server=1 on
// that thread so its rpc id stays 6 (the bed's single server slot).
// ---------------------------------------------------------------------------
TEST_F(MakoMigrationReroute, RacingWriterLosesNoAcknowledgedWriteAcrossMigration) {
    janus::get_migration_guard().clear();
    const std::string lo = "r10", hi = "r20";
    bool op = false;

    // Seed base rows so the migration moves something even if the writer is slow.
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(TThread::sclient->nontxnPut(kTableId, "r1" + std::to_string(i), "seed", &op),
                  static_cast<int>(mako::ErrorCode::SUCCESS));
    }

    // The racing writer: unique keys inside [r10, r20), recording every outcome.
    struct Outcome { std::string key, val; int status; };
    std::vector<Outcome> outcomes;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        TThread::set_num_eprc_server(1);   // rpc id stays 6 for par_id 1
        TThread::set_nshards(2);
        mako::ShardClient wclient(config_path(), "localhost", kClientShard, /*par_id=*/1);
        int i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            char kb[16], vb[16];
            snprintf(kb, sizeof kb, "r1%d_%04d", i % 10, i);   // in [r10, r20)
            snprintf(vb, sizeof vb, "w%04d", i);
            bool wop = false;
            int st;
            try {
                st = wclient.nontxnPut(kTableId, kb, vb, &wop);
            } catch (...) {
                // Client-side transport timeout etc.: the write's fate is
                // UNKNOWN (not acked) -- record as neither SUCCESS nor BUSY.
                st = -1;
            }
            outcomes.push_back(Outcome{kb, vb, st});
            ++i;
        }
    });

    // Let some writes land pre-fence, then migrate [r10,r20) 0 -> 1 with the
    // full corrected sequence on real engine participants.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    MigrationProbe probe;
    bool drain_ok = false;
    std::thread mig([&] {
        scoped_db_thread_ctx ctx(g_db, /*loader=*/true);
        janus::OrderedIndexShardData sd0(g_srv[0].store);
        janus::OrderedIndexShardData sd1(g_srv[1].store);
        janus::ShardMaster master =
            janus::ShardMaster::new_(g_cm, &janus::get_cluster_config());
        master.attach_shard(0, &sd0);
        master.attach_shard(1, &sd1);
        probe.begin_ok = master.begin_migration(0, 1, g_table_name, lo, hi);
        master.background_copy();          // live copy (writer racing)
        master.lock_range();               // staging fence up
        drain_ok = master.drain_source();  // wait out pre-fence in-flight writes
        // Hold the fence for a beat (models a long catch-up copy on a big
        // range) so the racing writer demonstrably hits it: without this the
        // whole fence window is a few ms and the per-RPC writer can miss it,
        // leaving the busy>0 coverage assertion flaky. Safety is timing-free;
        // this only widens the exercised window.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        master.background_copy();          // catch-up over the quiescent range
        master.final_sync();
        probe.prepared_ok = master.both_prepared();
        probe.commit_ok = master.commit_migration();
    });
    mig.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // post-cutover acks
    stop.store(true);
    writer.join();

    ASSERT_TRUE(probe.begin_ok);
    EXPECT_TRUE(drain_ok) << "the epoch drain must complete (advancer running)";
    ASSERT_TRUE(probe.prepared_ok)
        << "checksums must match after fence+drain+catch-up -- a mismatch here "
           "means a write slipped between the copy and the scans";
    ASSERT_TRUE(probe.commit_ok);

    // The safety property: every ACKED write is present with its value NOW
    // (the client reroutes to the final owner). SERVER_BUSY writes never
    // landed -- retried now, they land on the new owner.
    int acked = 0, busy = 0;
    for (const auto& o : outcomes) {
        if (o.status == static_cast<int>(mako::ErrorCode::SUCCESS)) {
            ++acked;
            std::string out;
            ASSERT_EQ(TThread::sclient->nontxnGet(kTableId, o.key, out),
                      static_cast<int>(mako::ErrorCode::SUCCESS))
                << "ACKED write lost: " << o.key;
            EXPECT_EQ(out, o.val) << "ACKED value corrupted: " << o.key;
        } else if (o.status == static_cast<int>(mako::ErrorCode::SERVER_BUSY)) {
            ++busy;
            std::string out;
            EXPECT_NE(TThread::sclient->nontxnGet(kTableId, o.key, out),
                      static_cast<int>(mako::ErrorCode::SUCCESS))
                << "SERVER_BUSY write must not have landed: " << o.key;
            bool rop = false;
            EXPECT_EQ(TThread::sclient->nontxnPut(kTableId, o.key, o.val, &rop),
                      static_cast<int>(mako::ErrorCode::SUCCESS))
                << "retry after cutover must land on the new owner";
        }
    }
    // The race must actually have been exercised on both sides of the fence.
    EXPECT_GT(acked, 0) << "no writes acked -- writer never got going";
    EXPECT_GT(busy, 0) << "no writes fenced -- the migration won before the "
                          "writer raced it (lengthen the writer head start?)";

    janus::get_migration_guard().clear();
}

}  // namespace
