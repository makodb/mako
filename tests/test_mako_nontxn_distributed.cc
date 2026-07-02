// Distributed gating tests for docs/mako-nontxn-api-plan.md.
//
// Two shards in ONE process, mirroring examples' ut/simpleShards.cc:
//   - Server role: FastTransport bound to shard 1's URI + ShardServer
//     with its own LOCAL-view table objects, running in detached
//     threads (event-driven).
//   - Client role: the gtest main thread with TThread::sclient set up,
//     driving the REMOTE-view table objects.
//
// The client's table objects carry is_remote=true, so the non-txn ops
// take the Phase-2 remote branch (nontxnPut/nontxnInsert/nontxnRemove
// RPCs + remoteGet), travel over the loopback UDP fake transport, and
// land in ShardReceiver::HandleNontxnWriteRequest, which runs the op
// through the server-side LOCAL non-txn path (one-op OCC txn).
// Verification reads go directly to the server-view table object.
//
// Table id 201 → shard (201-1)/200 = 1 under the table-ID-based
// routing fallback in compute_shard_for_key.

#include <stdlib.h>
#include <unistd.h>

#include "benchmarks/bench.h"
#include "benchmarks/mbta_wrapper.hh"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/shardClient.h"
#include "benchmarks/sto/Transaction.hh"
#include "benchmarks/sto/sync_util.hh"

#include <gtest/gtest.h>

import std;

namespace {

constexpr int kClientShard = 0;
constexpr int kServerShard = 1;
constexpr int kParId = 0;
constexpr int kNumWarehouses = 1;
// (id-1)/200 == 1 → owned by shard 1 per the table-ID routing fallback.
constexpr long kRemoteTableId = 201;

FastTransport* g_server_transport = nullptr;
transport::Configuration* g_config = nullptr;
mbta_wrapper* g_db = nullptr;

// The same logical table seen from the two roles:
mbta_ordered_index* g_client_tbl = nullptr;  // is_remote=true  → RPC path
mbta_ordered_index* g_server_tbl = nullptr;  // is_remote=false → local store

std::string config_path() {
    const char* candidates[] = {
        "./src/mako/config/local-shards2-warehouses1.yml",   // repo root
        "../src/mako/config/local-shards2-warehouses1.yml",  // build dir
    };
    for (const char* c : candidates) {
        if (access(c, R_OK) == 0) return c;
    }
    ADD_FAILURE() << "config yml not found from cwd";
    return candidates[0];
}

// Binds the fake (UDP) transport to shard 1's URI, wires the helper
// queues for client warehouse 0, and runs the event loop. Mirrors the
// production erpc_server in benchmarks/rpc_setup.cc (handler range
// 1..17 inclusive covers the non-txn types 14-17; rpc id = warehouses
// + 5 + alpha matches what Invoke* computes as the destination).
void erpc_server_thread(std::string cluster, transport::Configuration* config) {
    std::string local_uri =
        config->shard(kServerShard, mako::convertCluster(cluster)).host;
    int id = kNumWarehouses + 5 + 0;  // base=5, alpha=0
    g_server_transport = new FastTransport(config->configFile,
                                           local_uri,
                                           cluster,
                                           1, 17,
                                           0,   // physPort
                                           0,   // numa node
                                           kServerShard,
                                           id);
    std::unordered_map<uint16_t, mako::HelperQueue*> queues;
    std::unordered_map<uint16_t, mako::HelperQueue*> queues_response;
    // Key 0 = the requesting client's global warehouse id
    // (shard 0, par 0 with 1 warehouse per shard).
    queues[0] = new mako::HelperQueue(0, true);
    queues_response[0] = new mako::HelperQueue(0, false);
    g_server_transport->SetHelperQueues(queues);
    g_server_transport->SetHelperQueuesResponse(queues_response);
    g_server_transport->Run();
}

// The worker servicing requests against the registered (local-view)
// tables. Mirrors the production helper_server in
// benchmarks/rpc_setup.cc with g_wid=1 (client warehouse 0).
void helper_server_thread(transport::Configuration* config,
                          abstract_db* db,
                          std::map<int, abstract_ordered_index*> open_tables) {
    scoped_db_thread_ctx ctx(db, true, 1);
    TThread::set_mode(1);
    TThread::enable_multiverison();
    TThread::set_shard_index(kServerShard);
    TThread::set_pid(kParId);
    TThread::set_nshards(config->nshards);
    auto* ss = new mako::ShardServer(config->configFile,
                                     kServerShard,
                                     kClientShard, kParId);
    ss->Register(db,
                 g_server_transport->GetHelperQueue(0),
                 g_server_transport->GetHelperQueueResponse(0),
                 open_tables);
    ss->Run();  // event driven
}

class MakoNontxnDistributed : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        std::string path = config_path();
        g_config = new transport::Configuration(path);
        BenchmarkConfig::getInstance().setConfig(g_config);

        // Under FAIL_NEW_VERSION, InvokeGet (client) and
        // HandleGetRequest (server) consult the sync-util term
        // callbacks; unregistered std::functions throw
        // bad_function_call. No epoch changes in this test: term 0.
        register_sync_util_sc([]() { return 0; });
        register_sync_util_ss([]() { return 0; });

        g_db = new mbta_wrapper;

        {
            // Registers the gtest main thread with Silo/masstree
            // (loader mode avoids the ShardClient bring-up inside
            // thread_init).
            scoped_db_thread_ctx ctx(g_db, /*loader=*/true);
        }

        // The same logical table from the two role perspectives.
        g_client_tbl = new mbta_ordered_index(
            "nontxn_dist", kRemoteTableId, g_db, /*is_remote=*/true);
        g_server_tbl = new mbta_ordered_index(
            "nontxn_dist", kRemoteTableId, g_db, /*is_remote=*/false);

        // Server role in detached threads (transport first, then the
        // worker that consumes its queues).
        std::thread t1(erpc_server_thread, std::string("localhost"), g_config);
        t1.detach();
        sleep(2);
        std::map<int, abstract_ordered_index*> open_tables_by_id;
        open_tables_by_id[kRemoteTableId] = g_server_tbl;
        std::thread t2(helper_server_thread, g_config, g_db,
                       open_tables_by_id);
        t2.detach();
        sleep(1);

        // Client role on this thread.
        TThread::sclient = new mako::ShardClient(g_config->configFile,
                                                 "localhost",
                                                 kClientShard,
                                                 kParId);
    }
};

// ---------------------------------------------------------------------------
// Remote put → verify on the server-side (local) view.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemotePutRoundTrip) {
    const std::string val = mako::Encode("dist-v1");
    EXPECT_TRUE(g_client_tbl->put(lcdf::Str("dk1"), val));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk1"), out));
    EXPECT_EQ(out, "dist-v1");
}

TEST_F(MakoNontxnDistributed, RemotePutOverwrites) {
    EXPECT_TRUE(g_client_tbl->put(lcdf::Str("dk2"), mako::Encode("one")));
    EXPECT_FALSE(g_client_tbl->put(lcdf::Str("dk2"), mako::Encode("two")));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk2"), out));
    EXPECT_EQ(out, "two");
}

TEST_F(MakoNontxnDistributed, RemoteInsertIsExclusive) {
    EXPECT_TRUE(g_client_tbl->insert(lcdf::Str("dk3"), mako::Encode("first")));
    EXPECT_FALSE(g_client_tbl->insert(lcdf::Str("dk3"), mako::Encode("second")));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk3"), out));
    EXPECT_EQ(out, "first");
}

TEST_F(MakoNontxnDistributed, RemoteRemoveSemantics) {
    ASSERT_TRUE(g_client_tbl->put(lcdf::Str("dk4"), mako::Encode("victim")));

    EXPECT_TRUE(g_client_tbl->remove(lcdf::Str("dk4")));
    std::string out;
    EXPECT_FALSE(g_server_tbl->get(lcdf::Str("dk4"), out));

    EXPECT_FALSE(g_client_tbl->remove(lcdf::Str("dk4")));  // absent
}

// ---------------------------------------------------------------------------
// Remote get: write locally on the server view, read through the
// client view (remoteGet RPC path).
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemoteGetReadsServerState) {
    ASSERT_TRUE(g_server_tbl->put(lcdf::Str("dk5"), mako::Encode("server-owned")));

    std::string out;
    ASSERT_TRUE(g_client_tbl->get(lcdf::Str("dk5"), out));
    EXPECT_EQ(out, "server-owned");

    EXPECT_FALSE(g_client_tbl->get(lcdf::Str("dk5-missing"), out));
}

// ---------------------------------------------------------------------------
// Mixed: interleave remote non-txn ops and confirm the sequence is
// observed consistently on the owning shard.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemoteOpSequence) {
    for (int i = 0; i < 20; i++) {
        std::string k = "seq_" + std::to_string(i);
        ASSERT_TRUE(g_client_tbl->put(lcdf::Str(k), mako::Encode("v" + std::to_string(i))));
    }
    for (int i = 0; i < 20; i += 2) {
        std::string k = "seq_" + std::to_string(i);
        ASSERT_TRUE(g_client_tbl->remove(lcdf::Str(k)));
    }
    for (int i = 0; i < 20; i++) {
        std::string k = "seq_" + std::to_string(i);
        std::string out;
        bool found = g_server_tbl->get(lcdf::Str(k), out);
        if (i % 2 == 0) {
            EXPECT_FALSE(found) << k;
        } else {
            ASSERT_TRUE(found) << k;
            EXPECT_EQ(out, "v" + std::to_string(i));
        }
    }
}

}  // namespace
