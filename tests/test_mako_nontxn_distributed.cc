// Distributed gating tests for docs/storage-interface.md.
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
// RPCs + remoteGet), travel over real loopback srpc/TCP, and
// land in ShardReceiver::HandleNontxnWriteRequest, which runs the op
// through the server-side LOCAL non-txn path (one-op OCC txn).
// Verification reads go directly to the server-view table object.
//
// Table id 201 → shard (201-1)/200 = 1 under the table-ID-based
// routing fallback in compute_shard_for_key.

#include <stdlib.h>
#include <unistd.h>

#include "benchmarks/bench.h"
#include "storage/mbta_wrapper.hh"
#include "storage/mbta_sharded_ordered_index.hh"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/shardClient.h"
#include "lib/table_registry.h"
#include "rocks_interface/client_tcp_server.h"
#include "rocks_interface/local_table.hh"
#include "rocks_interface/remote_db.hh"
#include "sto/Transaction.hh"
#include "sto/sync_util.hh"

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
#ifdef MAKO_SOURCE_DIR
        // Out-of-tree build dirs (the pristine gate tree) resolve via the
        // baked source root; the relative forms only work when the build
        // dir nests inside the source tree.
        MAKO_SOURCE_DIR "/src/mako/config/local-shards2-warehouses1.yml",
#endif
        "./src/mako/config/local-shards2-warehouses1.yml",   // repo root
        "../src/mako/config/local-shards2-warehouses1.yml",  // build dir
    };
    for (const char* c : candidates) {
        if (access(c, R_OK) == 0) return c;
    }
    ADD_FAILURE() << "config yml not found from cwd";
    return candidates[0];
}

// Binds the real srpc transport to shard 1's URI, wires the helper
// queues for client warehouse 0, and runs the event loop. Mirrors the
// production rpc_server in benchmarks/rpc_setup.cc (handler range
// 1..17 inclusive covers the non-txn types 14-17; rpc id = warehouses
// + 5 + alpha matches what Invoke* computes as the destination).
void rpc_server_thread(std::string cluster, transport::Configuration* config) {
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
    // @unsafe - install the same static geometry and registry used by real endpoints.
    static void SetUpTestSuite() {
        std::string path = config_path();
        g_config = new transport::Configuration(path);
        BenchmarkConfig::getInstance().setConfig(g_config);
        BenchmarkConfig::getInstance().setNshards(g_config->nshards);
        BenchmarkConfig::getInstance().setNthreads(kNumWarehouses);
        BenchmarkConfig::getInstance().setShardIndex(kClientShard);
        BenchmarkConfig::getInstance().setIsReplicated(0);
        BenchmarkConfig::getInstance().setCluster("localhost");

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
        g_client_tbl = mbta_index_build("nontxn_dist", kRemoteTableId,
                                        /*is_remote=*/true);
        g_server_tbl = mbta_index_build("nontxn_dist", kRemoteTableId,
                                        /*is_remote=*/false);
        mako::get_table_registry().register_table(kRemoteTableId, "nontxn_dist",
                                                  kServerShard, true, g_server_tbl);

        // Server role in detached threads (transport first, then the
        // worker that consumes its queues).
        std::thread t1(rpc_server_thread, std::string("localhost"), g_config);
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
        // Direct verification reads must decode the participant's retained versions.
        TThread::enable_multiverison();
    }
};

// ---------------------------------------------------------------------------
// Remote put → verify on the server-side (local) view.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemotePutRoundTrip) {
    const std::string val = "dist-v1";
    EXPECT_TRUE(g_client_tbl->put(lcdf::Str("dk1"), val));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk1"), out, std::string::npos));
    EXPECT_EQ(out, "dist-v1");
}

TEST_F(MakoNontxnDistributed, RemotePutOverwrites) {
    EXPECT_TRUE(g_client_tbl->put(lcdf::Str("dk2"), "one"));
    EXPECT_FALSE(g_client_tbl->put(lcdf::Str("dk2"), "two"));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk2"), out, std::string::npos));
    EXPECT_EQ(out, "two");
}

TEST_F(MakoNontxnDistributed, RemoteInsertIsExclusive) {
    EXPECT_TRUE(g_client_tbl->insert(lcdf::Str("dk3"), "first"));
    EXPECT_FALSE(g_client_tbl->insert(lcdf::Str("dk3"), "second"));

    std::string out;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("dk3"), out, std::string::npos));
    EXPECT_EQ(out, "first");
}

TEST_F(MakoNontxnDistributed, RemoteRemoveSemantics) {
    ASSERT_TRUE(g_client_tbl->put(lcdf::Str("dk4"), "victim"));

    EXPECT_TRUE(g_client_tbl->remove(lcdf::Str("dk4")));
    std::string out;
    EXPECT_FALSE(g_server_tbl->get(lcdf::Str("dk4"), out, std::string::npos));

    EXPECT_FALSE(g_client_tbl->remove(lcdf::Str("dk4")));  // absent
}

// @unsafe - real proxy, RPC participant, and OCC terminal paths; no fake replies.
TEST_F(MakoNontxnDistributed, RemoteTransactionalDeleteCommitsAndAborts) {
    TThread::set_mode(0);
    TThread::set_shard_index(kClientShard);
    TThread::set_pid(kParId);
    TThread::set_nshards(g_config->nshards);
    TThread::in_loading_phase = false;
    const lcdf::Str key("transactional-delete");
    ASSERT_TRUE(g_server_tbl->put(key, "original"));
    const std::string replacement = mako::Encode(std::string_view("replacement"));
    // @unsafe - stage through the public transactional API and finish all peers.
    auto remove = [&](bool commit, bool put_first) {
        mako::ShardingOperation operation;
        Sto::start_transaction();
        try {
            if (put_first) g_client_tbl->tx_put(nullptr, key, replacement);
            g_client_tbl->tx_remove(nullptr, key);
            if (commit && Sto::try_commit()) return true;
            Sto::abort_without_throw();
            return false;
        } catch (...) {
            Sto::abort_without_throw();
            throw;
        }
    };
    EXPECT_FALSE(remove(false, false));
    std::string value;
    ASSERT_TRUE(g_server_tbl->get(key, value, std::string::npos));
    EXPECT_EQ(value, "original");
    // Aborting the first unseen-key delete must not poison its proxy carrier.
    ASSERT_TRUE(remove(true, false));
    EXPECT_FALSE(g_server_tbl->get(key, value, std::string::npos));
    g_server_tbl->put(key, "recreated");
    ASSERT_TRUE(g_server_tbl->get(key, value, std::string::npos));
    EXPECT_EQ(value, "recreated");
    ASSERT_TRUE(remove(true, true));
    EXPECT_FALSE(g_server_tbl->get(key, value, std::string::npos));
}

// ---------------------------------------------------------------------------
// Remote get: write locally on the server view, read through the
// client view (remoteGet RPC path).
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemoteGetReadsServerState) {
    ASSERT_TRUE(g_server_tbl->put(lcdf::Str("dk5"), "server-owned"));

    std::string out;
    ASSERT_TRUE(g_client_tbl->get(lcdf::Str("dk5"), out, std::string::npos));
    EXPECT_EQ(out, "server-owned");

    EXPECT_FALSE(g_client_tbl->get(lcdf::Str("dk5-missing"), out, std::string::npos));

    // Regression: a value LONGER than EXTRA_BITS_FOR_VALUE. The server
    // strips the suffix once (L3 get); a second client-side strip
    // would silently truncate long values (short ones dodge the bug).
    const std::string long_val(4 * mako::EXTRA_BITS_FOR_VALUE, 'x');
    ASSERT_TRUE(g_server_tbl->put(lcdf::Str("dk5-long"), long_val));
    ASSERT_TRUE(g_client_tbl->get(lcdf::Str("dk5-long"), out, std::string::npos));
    EXPECT_EQ(out, long_val);
}

// ---------------------------------------------------------------------------
// Mixed: interleave remote non-txn ops and confirm the sequence is
// observed consistently on the owning shard.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, RemoteOpSequence) {
    for (int i = 0; i < 20; i++) {
        std::string k = "seq_" + std::to_string(i);
        ASSERT_TRUE(g_client_tbl->put(lcdf::Str(k), "v" + std::to_string(i)));
    }
    for (int i = 0; i < 20; i += 2) {
        std::string k = "seq_" + std::to_string(i);
        ASSERT_TRUE(g_client_tbl->remove(lcdf::Str(k)));
    }
    for (int i = 0; i < 20; i++) {
        std::string k = "seq_" + std::to_string(i);
        std::string out;
        bool found = g_server_tbl->get(lcdf::Str(k), out, std::string::npos);
        if (i % 2 == 0) {
            EXPECT_FALSE(found) << k;
        } else {
            ASSERT_TRUE(found) << k;
            EXPECT_EQ(out, "v" + std::to_string(i));
        }
    }
}

// ---------------------------------------------------------------------------
// L7 facade: ITable non-txn surface over the same store.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, L7LocalTableNontxn) {
    auto* sharded = new mbta_sharded_ordered_index(
        "nontxn_dist", std::vector<abstract_ordered_index*>{g_server_tbl});
    mako::LocalTable lt(sharded, "nontxn_dist");

    EXPECT_TRUE(lt.Put("l7k1", "v1").ok());
    std::string out;
    ASSERT_TRUE(lt.Get("l7k1", out).ok());
    EXPECT_EQ(out, "v1");

    EXPECT_TRUE(lt.Insert("l7k2", "first").ok());
    EXPECT_TRUE(lt.Insert("l7k2", "second").IsInvalidArgument());
    ASSERT_TRUE(lt.Get("l7k2", out).ok());
    EXPECT_EQ(out, "first");

    bool exists = false;
    EXPECT_TRUE(lt.Exists("l7k2", &exists).ok());
    EXPECT_TRUE(exists);

    EXPECT_TRUE(lt.Delete("l7k2").ok());
    EXPECT_TRUE(lt.Delete("l7k2").IsNotFound());
    EXPECT_TRUE(lt.Exists("l7k2", &exists).ok());
    EXPECT_FALSE(exists);
    EXPECT_TRUE(lt.Get("l7k2", out).IsNotFound());
}

// End-to-end decoupled-client path: RemoteDB's raw KV socket →
// ClientTcpServer → ShardReceiver::RunNontxnOp → L3 non-txn ops on
// the server table. This is the re-based (sound) RemoteTable KV path;
// the old one staged shard_put writes that never committed.
TEST_F(MakoNontxnDistributed, L7RemoteTableNontxn) {
    auto* recv = new mako::ShardReceiver(config_path());
    std::map<int, abstract_ordered_index*> tables;
    tables[kRemoteTableId] = g_server_tbl;
    recv->Register(g_db, tables);

    auto* tcp = new mako::ClientTcpServer(31307, 2);
    tcp->SetReceiver(recv);
    ASSERT_TRUE(tcp->Start());

    mako::RemoteDB* rdb = nullptr;
    ASSERT_TRUE(mako::RemoteDB::ConnectNontxn("127.0.0.1", 31307,
        UINT64_C(0x8000000000000307), &rdb).ok());
    mako::ITable* tbl = rdb->GetTable("nontxn_dist", kRemoteTableId);
    ASSERT_NE(tbl, nullptr);

    EXPECT_TRUE(tbl->Put("l7r1", "remote-v1").ok());
    std::string out;
    ASSERT_TRUE(tbl->Get("l7r1", out).ok());
    EXPECT_EQ(out, "remote-v1");

    EXPECT_TRUE(tbl->Insert("l7r2", "only").ok());
    EXPECT_TRUE(tbl->Insert("l7r2", "dup").IsInvalidArgument());

    bool exists = false;
    EXPECT_TRUE(tbl->Exists("l7r2", &exists).ok());
    EXPECT_TRUE(exists);

    EXPECT_TRUE(tbl->Delete("l7r2").ok());
    EXPECT_TRUE(tbl->Delete("l7r2").IsNotFound());
    EXPECT_TRUE(tbl->Get("l7r2", out).IsNotFound());

    // Long value survives the round trip (single server-side strip).
    const std::string long_val(4 * mako::EXTRA_BITS_FOR_VALUE, 'y');
    EXPECT_TRUE(tbl->Put("l7r3", long_val).ok());
    ASSERT_TRUE(tbl->Get("l7r3", out).ok());
    EXPECT_EQ(out, long_val);

    // Writes are REAL: visible + committed on the server-side view
    // (the old shard_put path staged uncommitted, invisible writes).
    std::string sv;
    ASSERT_TRUE(g_server_tbl->get(lcdf::Str("l7r1"), sv, std::string::npos));
    EXPECT_EQ(sv, "remote-v1");

    // Session rollback closes tracking, not already committed gateway writes.
    void* session = rdb->BeginTransaction();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(tbl->Put(session, "l7-session", "already-committed").ok());
    ASSERT_TRUE(rdb->RollbackStatus(session).ok());
    ASSERT_TRUE(tbl->Get("l7-session", out).ok());
    EXPECT_EQ(out, "already-committed");

    rdb->Disconnect();
    delete rdb;
    tcp->Stop();
}

// @unsafe - exercises the REAL receiver/RunNontxnOp/store path. Ignoring the
// first reply models reply loss without replacing the storage callback.
TEST_F(MakoNontxnDistributed, GatewayRetainsActualTerminalReplies) {
    mako::ShardReceiver receiver(config_path());
    std::map<int, abstract_ordered_index*> tables{{kRemoteTableId, g_server_tbl}};
    receiver.Register(g_db, tables);
    MakoGatewayRequest request{};
    request.version = MAKO_GATEWAY_VERSION;
    request.kind = MAKO_GATEWAY_INSERT;
    request.client = UINT64_C(0x8000000000000310);
    request.sequence = 1;
    request.physical_table = kRemoteTableId;
    request.owner = kServerShard;
    request.route_known = 1;
    const std::string key = "gateway-lost-reply";
    std::memcpy(request.key, key.data(), key.size());
    request.key_length = key.size();
    request.value[0] = 'a'; request.value_length = 1;
    // The destination already executed this exact operation, but its successful
    // reply was lost before ingress retained it. Use the REAL native RPC sender.
    mako::ShardingRequest forwarded{};
    forwarded.transaction = {request.client, request.sequence};
    forwarded.grant = {kServerShard, 0};
    mako::ShardClient sender(config_path(), "localhost", kClientShard, 0, true);
    bool inserted = false;
    ASSERT_EQ(sender.forwardNontxn(forwarded, mako::nontxnInsertReqType,
        kRemoteTableId, key, "a", &inserted, nullptr), mako::ErrorCode::SUCCESS);
    ASSERT_TRUE(inserted);
    MakoGatewayResponse first{}, retry{};
    receiver.ClientOperation(request, first);
    ASSERT_EQ(first.status, mako::ErrorCode::SUCCESS);
    ASSERT_EQ(first.outcome, MAKO_GATEWAY_COMMITTED);
    ASSERT_EQ(first.op_result, 1u);
    // A fresh insert would return false. The retry must return the retained TRUE.
    receiver.ClientOperation(request, retry);
    EXPECT_EQ(retry.status, first.status);
    EXPECT_EQ(retry.outcome, first.outcome);
    EXPECT_EQ(retry.op_result, first.op_result);

    request.kind = MAKO_GATEWAY_GET;
    request.sequence = 2;
    request.value_length = 0;
    receiver.ClientOperation(request, first);
    ASSERT_EQ(first.status, mako::ErrorCode::SUCCESS);
    ASSERT_EQ(first.value_length, 1u);
    ASSERT_EQ(first.value[0], 'a');
    ASSERT_FALSE(g_server_tbl->put(lcdf::Str(key), "b"));
    receiver.ClientOperation(request, retry);
    ASSERT_EQ(retry.value_length, 1u);
    EXPECT_EQ(retry.value[0], 'a'); // not a reexecuted read of current storage

    request.kind = MAKO_GATEWAY_DELETE;
    request.sequence = 3;
    receiver.ClientOperation(request, first);
    ASSERT_EQ(first.status, mako::ErrorCode::SUCCESS);
    ASSERT_EQ(first.op_result, 1u);
    receiver.ClientOperation(request, retry);
    EXPECT_EQ(retry.op_result, 1u); // not a repeated delete of an absent row
    request.kind = MAKO_GATEWAY_INSERT;
    request.sequence = 1;
    request.value_length = 1;
    receiver.ClientOperation(request, retry);
    EXPECT_EQ(retry.outcome, MAKO_GATEWAY_REJECTED);
    std::string value;
    EXPECT_FALSE(g_server_tbl->get(lcdf::Str(key), value, std::string::npos));
}

}  // namespace
