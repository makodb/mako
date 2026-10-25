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
#include "benchmarks/tpcc.h"
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
#include "native_sharding_host.h"
#include <rusty/thread.hpp>

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
SiloRuntime* g_native_runtime = nullptr;

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
// 1..18 inclusive covers the non-txn types 14-17 and full scan pages;
// rpc id = warehouses + 5 + alpha matches the client's destination).
// @unsafe - legacy transport and helper-queue ownership lasts until process exit.
void rpc_server_thread(std::string cluster, transport::Configuration* config) {
    std::string local_uri =
        config->shard(kServerShard, mako::convertCluster(cluster)).host;
    int id = kNumWarehouses + 5 + 0;  // base=5, alpha=0
    g_server_transport = new FastTransport(config->configFile,
                                           local_uri,
                                           cluster,
                                           1, mako::fullScanReqType,
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
// @unsafe - legacy worker borrows process-lifetime runtime, DB and registered tables.
void helper_server_thread(transport::Configuration* config,
                          abstract_db* db,
                          std::map<int, abstract_ordered_index*> open_tables) {
    // @unsafe - native mode shares the registered engine runtime across workers.
    if (g_native_runtime) {
        BenchmarkConfig::setThreadLocalShardIndex(kServerShard);
        g_native_runtime->BindToCurrentThread();
    }
    scoped_db_thread_ctx ctx(db, true, 1);
    TThread::set_mode(1);
    TThread::enable_multiverison();
    TThread::set_shard_index(kServerShard);
    TThread::set_pid(kParId);
    TThread::set_nshards(config->nshards);
    TThread::in_loading_phase = false;
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
        // @unsafe - existing process-lifetime RPC worker owns its closure.
        auto t1 = rusty::thread::spawn([config = g_config]() {
            rpc_server_thread("localhost", config);
        });
        t1.detach();
        sleep(2);
        std::map<int, abstract_ordered_index*> open_tables_by_id;
        open_tables_by_id[kRemoteTableId] = g_server_tbl;
        // @unsafe - legacy registration map is the server ABI boundary.
        auto t2 = rusty::thread::spawn(
            [config = g_config, db = g_db, tables = std::move(open_tables_by_id)]() mutable {
                helper_server_thread(config, db, std::move(tables));
            });
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

// @unsafe - the static protocol treats a failed read reply as terminal and
// deliberately sends no Abort RPC. Its next request needs a fresh idle helper.
TEST_F(MakoNontxnDistributed, FailedReadRestoresIdleHelper) {
    const std::string key = "static-helper-live", value = "committed-helper-value";
    ASSERT_TRUE(g_server_tbl->put(lcdf::Str(key), value));
    const auto saved_mode = TThread::mode();
    const auto saved_shard = TThread::get_shard_index();
    TThread::set_mode(1);
    TThread::set_shard_index(kServerShard);
    mako::ShardReceiver receiver(config_path());
    receiver.Register(g_db, {{kRemoteTableId, g_server_tbl}});
    const std::string lo = "static-helper-missing-a", hi = "static-helper-missing-z";
    mako::scan_request_t scan{};
    scan.req_nr = 1;
    scan.table_id = kRemoteTableId;
    scan.slen = lo.size();
    scan.elen = hi.size();
    std::memcpy(scan.start_end_key, lo.data(), lo.size());
    std::memcpy(scan.start_end_key + lo.size(), hi.data(), hi.size());
    mako::scan_response_t scan_reply;
    receiver.ReceiveRequest(mako::scanReqType,
        reinterpret_cast<char*>(&scan), reinterpret_cast<char*>(&scan_reply));
    EXPECT_EQ(scan_reply.status, mako::ErrorCode::ABORT);
    EXPECT_TRUE(TThread::txn->in_progress());
    EXPECT_FALSE(TThread::txn->has_epoch());
    const auto idle_cut = Transaction::epoch_cut();
    EXPECT_NE(idle_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(idle_cut));

    // No terminal Abort between the failed scan and this next transaction.
    mako::get_request_t get{};
    get.req_nr = 2;
    get.table_id = kRemoteTableId;
    get.len = key.size();
    std::memcpy(get.key, key.data(), key.size());
    mako::get_response_t get_reply;
    receiver.ReceiveRequest(mako::getReqType,
        reinterpret_cast<char*>(&get), reinterpret_cast<char*>(&get_reply));
    EXPECT_EQ(get_reply.status, mako::ErrorCode::SUCCESS);
    EXPECT_GE(get_reply.len, value.size());
    if (get_reply.len >= value.size())
        EXPECT_EQ(std::string(get_reply.value, value.size()), value);
    mako::basic_request_t abort{};
    abort.req_nr = 3;
    mako::basic_response_t abort_reply;
    receiver.ReceiveRequest(mako::abortReqType,
        reinterpret_cast<char*>(&abort), reinterpret_cast<char*>(&abort_reply));
    EXPECT_EQ(abort_reply.status, mako::ErrorCode::SUCCESS);
    Sto::silent_abort();
    TThread::set_shard_index(saved_shard);
    TThread::set_mode(saved_mode);
    EXPECT_TRUE(g_server_tbl->remove(lcdf::Str(key)));
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
    request.snapshot_version = MAKO_GATEWAY_NO_SNAPSHOT;
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
    forwarded.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
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

// This mode is isolated because native catalog/actor installation is process
// global. Run only this test with MAKO_NATIVE_MIXED_SCAN=1 MAKO_CLUSTER_CONFIG=1.
// The ordinary static-routing suite continues to use its original bootstrap.
struct NativeMixedHosts {
    rusty::Vec<rusty::Box<mako::NativeShardingHost>> hosts;
    // @unsafe - close listeners before joining native jobs and dropping callbacks.
    ~NativeMixedHosts() {
        for (auto& host : hosts) host->stop_accepting();
        for (auto& host : hosts) mako_sharding_stop_node(host->owner);
    }
};

// @unsafe - synchronous native ABI borrows the string bytes, never owns them.
MakoShardBytes native_bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// @unsafe - use the real connected native srpc sender for administration as well.
uint32_t native_admin_call(void* host, uint32_t operation, MakoShardBytes payload,
                           MakoShardReplySink sink, void* context) {
    return mako::native_sharding_peer_call(host, 0, operation, payload, sink, context);
}

// @unsafe - the opaque identity is observed only under a real STO/native guard;
// its routing and point admission are checked independently of the identity.
uint64_t native_cache_view(uint32_t owner, uint64_t table, const std::string& point) {
    const auto epoch = Transaction::epoch_enter();
    EXPECT_NE(epoch, 0u);
    if (!epoch) return 0;
    const MakoShardRead* read = nullptr;
    const auto status = mako_sharding_read_begin(owner, epoch, &read);
    EXPECT_EQ(status, MAKO_SHARD_OK);
    uint64_t identity = 0;
    if (status == MAKO_SHARD_OK) {
        EXPECT_EQ(mako_sharding_read_view_id(read, &identity), MAKO_SHARD_OK);
        EXPECT_NE(identity, 0u);
        MakoShardRoute route{};
        EXPECT_EQ(mako_sharding_read_route(read, table, native_bytes(point), &route), MAKO_SHARD_OK);
        EXPECT_EQ(mako_sharding_read_point(read, table, native_bytes(point), route), MAKO_SHARD_OK);
        mako_sharding_read_end(read);
    }
    Transaction::epoch_leave();
    return identity;
}

struct NativeMixedCallbackFailure {};
struct NativeMixedRows final : oi_scan_callback {
    rusty::Vec<std::pair<std::string, std::string>> rows;
    rusty::Vec<const std::string*> values;
    size_t stop = SIZE_MAX;
    bool fail = false;
    bool retain = false;
    // @unsafe - match the engine callback's nonthrowing destruction contract.
    ~NativeMixedRows() noexcept override = default;
    // @unsafe - synchronous key borrow; values remain borrowed only with an arena.
    bool invoke(const char* key, size_t length, const std::string& value) override {
        rows.push(std::make_pair(std::string(key, length), value));
        if (retain) values.push(&value);
        if (rows.size() == stop) {
            if (fail) throw NativeMixedCallbackFailure{};
            return false;
        }
        return true;
    }
};

// @unsafe - real MBTA/STO, production Rust migration, and ordinary srpc scan RPCs.
TEST(NativeMixedScan, LocalAndRemoteSegments) {
    if (!std::getenv("MAKO_NATIVE_MIXED_SCAN"))
        GTEST_SKIP() << "isolated native mode: set MAKO_NATIVE_MIXED_SCAN=1 and filter NativeMixedScan.*";
    ASSERT_EQ(g_db, nullptr) << "native mode must run alone, without the static fixture";
    constexpr uint64_t canonical = 1001;
    const std::string name = "native_mixed_scan", lo = "m", hi = "n";
    const std::string empty, binary("b\0", 2), successor("b\0\0", 3), high("\xff\0\xff", 3);
    std::string oversized(mako::full_scan_page_capacity + 1024, 'L');
    oversized.front() = '\0';
    oversized.back() = '\xff';
    g_config = new transport::Configuration(config_path());
    g_config->multi_shard_mode = true;
    g_config->local_shard_indices = {0, 1};
    auto& bench = BenchmarkConfig::getInstance();
    bench.setConfig(g_config);
    bench.setNshards(2);
    bench.setNthreads(1);
    bench.setShardIndex(0);
    bench.setIsReplicated(0);
    bench.setIsMicro(0);
    bench.setCluster("localhost");
    bench.setPaxosProcName("localhost");
    bench.setPinCpus(0);
    register_sync_util_sc([]() { return 0; });
    register_sync_util_ss([]() { return 0; });
    g_native_runtime = SiloRuntime::Current();
    g_native_runtime->BindToCurrentThread();
    g_db = new mbta_wrapper;
    g_db->init();
    g_db->thread_init(true, 0);
    ASSERT_EQ(mako_sharding_catalog_register(canonical, native_bytes(name), 0, 0), MAKO_SHARD_OK);
    ASSERT_EQ(mako_sharding_warehouse_init(1, 2), MAKO_SHARD_OK);
    for (unsigned owner = 0; owner < 2; ++owner) {
        BenchmarkConfig::setThreadLocalShardIndex(owner);
        TThread::set_shard_index(owner);
        auto* index = dynamic_cast<mbta_ordered_index*>(g_db->open_index(name, int(owner)));
        ASSERT_NE(index, nullptr);
        if (owner == 0) {
            g_client_tbl = index;
            ASSERT_TRUE(mako::get_table_registry().bind_native(name, canonical, rusty::None));
        } else {
            g_server_tbl = index;
        }
        ASSERT_EQ(mako_sharding_raw_register(canonical, owner, 0,
            reinterpret_cast<uintptr_t>(index)), MAKO_SHARD_OK);
    }
    BenchmarkConfig::setThreadLocalShardIndex(0);
    TThread::set_shard_index(0);
    NativeMixedRows expected;
    expected.rows.push({empty, "empty-key"});
    expected.rows.push({binary, oversized});
    expected.rows.push({successor, "binary-successor"});
    for (unsigned i = 0; i < 40; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "m%04u", i);
        expected.rows.push({key, std::string(4096, char('A' + i % 26))});
    }
    expected.rows.push({"z", "last-local"});
    expected.rows.push({high, "high-key"});
    for (const auto& row : expected.rows)
        ASSERT_TRUE(g_client_tbl->put(lcdf::Str(row.first), row.second));
    // @unsafe - admission is unavailable before native startup, but a real
    // loading transaction already owns an engine guard. Failed logical begin
    // must release only its extra guard, never that engine announcement.
    TThread::set_mode(0);
    Sto::start_transaction();
    const auto failed_bind_cut = Transaction::epoch_cut();
    EXPECT_NE(failed_bind_cut, 0u);
    TThread::in_loading_phase = false;
    mako::ShardingRequest unavailable{};
    unavailable.transaction = {0x45504f4342494e44ULL, 1};
    unavailable.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
    EXPECT_FALSE(mako::sharding_bind_request(unavailable));
    EXPECT_FALSE(mako::sharding_has_request());
    EXPECT_FALSE(Transaction::epoch_drained(failed_bind_cut));
    Sto::silent_abort();
    EXPECT_TRUE(Transaction::epoch_drained(failed_bind_cut));
    TThread::in_loading_phase = false;
    NativeMixedHosts native;
    for (unsigned owner = 0; owner < 2; ++owner) {
        auto host = rusty::make_box<mako::NativeShardingHost>(g_db, owner, 2, g_native_runtime);
        auto callbacks = host->callbacks();
        ASSERT_EQ(mako_sharding_start_node(owner, 2, 1, &callbacks), MAKO_SHARD_OK);
        const auto listening = host->start_service();
        native.hosts.push(std::move(host));
        ASSERT_EQ(listening, MAKO_SHARD_OK);
    }
    for (unsigned owner = 0; owner < 2; ++owner) {
        for (unsigned attempt = 0; attempt < 200 && !mako_sharding_ready(owner); ++attempt)
            usleep(50000);
        ASSERT_TRUE(mako_sharding_ready(owner));
    }
    // @unsafe - nested operations and physical retries retain the oldest STO
    // announcement. Repeated captures and newly read absent keys remain legal
    // after a cut; commit never compares the operation's epoch with the clock.
    uint64_t retry_cut = 0;
    {
        mako::ShardingOperation operation;
        std::string value;
        EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), value, std::string::npos));
        retry_cut = Transaction::epoch_cut();
        EXPECT_NE(retry_cut, 0u);
        EXPECT_FALSE(Transaction::epoch_drained(retry_cut));
        {
            mako::ShardingOperation nested;
            EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), value, std::string::npos));
        }
        EXPECT_FALSE(Transaction::epoch_drained(retry_cut));
        Sto::start_transaction();
        EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"), value, std::string::npos));
        Sto::silent_abort();
        EXPECT_FALSE(Transaction::epoch_drained(retry_cut));
        EXPECT_FALSE(g_client_tbl->get(lcdf::Str("epoch-absent"), value, std::string::npos));
        EXPECT_FALSE(Transaction::epoch_drained(retry_cut));
    }
    EXPECT_TRUE(Transaction::epoch_drained(retry_cut));

    // @unsafe - immutable table bindings may survive logical completion, but
    // raw-key captures and staged-batch membership may not.
    const auto cached_view = native_cache_view(0, canonical, "z");
    ASSERT_NE(cached_view, 0u);
    MakoShardRoute original_route{};
    ASSERT_EQ(mako_sharding_route(canonical, native_bytes(std::string("z")),
                                  &original_route), MAKO_SHARD_OK);
    mako::ShardingRequest earlier_request{};
    {
        mako::ShardingOperation operation;
        const auto point = mako::sharding_resolve_point(g_client_tbl->get_table_id(), "z", 1);
        const auto transaction = mako::sharding_transaction();
        std::string actual;
        EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), actual, std::string::npos));
        EXPECT_EQ(actual, "last-local");
        const std::string first_capture = "capture-only-0";
        const auto grown_point = mako::sharding_resolve_point(g_client_tbl->get_table_id(),
            first_capture.data(), first_capture.size());
        for (unsigned i = 1; i < 96; ++i) {
            const auto key = "capture-only-" + std::to_string(i);
            mako::sharding_resolve_point(g_client_tbl->get_table_id(), key.data(), key.size());
        }
        // Neither retained descriptor is serialized until the raw capture
        // vector has grown and a different row was resolved last.
        point.write_request(earlier_request);
        EXPECT_EQ(earlier_request.transaction.client, transaction.client);
        EXPECT_EQ(earlier_request.transaction.sequence, transaction.sequence);
        EXPECT_EQ(earlier_request.table, canonical);
        EXPECT_FALSE(earlier_request.fixed_coordinate);
        ASSERT_EQ(earlier_request.coordinate_length, 1u);
        EXPECT_EQ(earlier_request.coordinate[0], 'z');
        EXPECT_EQ(earlier_request.grant.owner, original_route.grant.owner);
        EXPECT_EQ(earlier_request.grant.ownership_version, original_route.grant.ownership_version);
        EXPECT_EQ(earlier_request.snapshot_version, original_route.snapshot_version);
        mako::ShardingRequest grown_request{};
        grown_point.write_request(grown_request);
        ASSERT_EQ(grown_request.coordinate_length, first_capture.size());
        EXPECT_EQ(std::memcmp(grown_request.coordinate, first_capture.data(), first_capture.size()), 0);
        EXPECT_EQ(grown_request.table, canonical);
        EXPECT_FALSE(grown_request.fixed_coordinate);
        EXPECT_EQ(grown_request.grant.owner, original_route.grant.owner);
        EXPECT_EQ(grown_request.grant.ownership_version, original_route.grant.ownership_version);
        EXPECT_EQ(grown_request.snapshot_version, original_route.snapshot_version);
        EXPECT_EQ(grown_request.transaction.client, transaction.client);
        EXPECT_EQ(grown_request.transaction.sequence, transaction.sequence);
        mako::ShardingRequest staged{};
        EXPECT_EQ(mako::sharding_batch_request(g_client_tbl->get_table_id(), "z", staged), 0);
        EXPECT_EQ(staged.table, earlier_request.table);
        EXPECT_EQ(staged.fixed_coordinate, earlier_request.fixed_coordinate);
        ASSERT_EQ(staged.coordinate_length, earlier_request.coordinate_length);
        EXPECT_EQ(std::memcmp(staged.coordinate, earlier_request.coordinate, staged.coordinate_length), 0);
        EXPECT_EQ(staged.grant.owner, earlier_request.grant.owner);
        EXPECT_EQ(staged.grant.ownership_version, earlier_request.grant.ownership_version);
        EXPECT_EQ(staged.snapshot_version, earlier_request.snapshot_version);
        EXPECT_EQ(staged.transaction.client, transaction.client);
        EXPECT_EQ(staged.transaction.sequence, transaction.sequence);
        EXPECT_EQ(mako::sharding_batch_request(
            g_client_tbl->get_table_id(), first_capture, staged), 0);
        EXPECT_EQ(staged.table, canonical);
        EXPECT_FALSE(staged.fixed_coordinate);
        ASSERT_EQ(staged.coordinate_length, first_capture.size());
        EXPECT_EQ(std::memcmp(staged.coordinate, first_capture.data(), first_capture.size()), 0);
        EXPECT_EQ(staged.grant.owner, grown_request.grant.owner);
        EXPECT_EQ(staged.grant.ownership_version, grown_request.grant.ownership_version);
        EXPECT_EQ(staged.snapshot_version, grown_request.snapshot_version);
        EXPECT_EQ(staged.transaction.client, transaction.client);
        EXPECT_EQ(staged.transaction.sequence, transaction.sequence);
        EXPECT_EQ(mako::sharding_batch_request(
            g_client_tbl->get_table_id(), "capture-only-95", staged), 0);
        point.write_request(staged);
        ASSERT_EQ(staged.coordinate_length, 1u);
        EXPECT_EQ(staged.coordinate[0], 'z');
        EXPECT_EQ(staged.grant.owner, original_route.grant.owner);
        EXPECT_EQ(staged.grant.ownership_version, original_route.grant.ownership_version);
        EXPECT_EQ(staged.snapshot_version, original_route.snapshot_version);
        EXPECT_EQ(staged.transaction.client, transaction.client);
        EXPECT_EQ(staged.transaction.sequence, transaction.sequence);
        EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), actual, std::string::npos));
        EXPECT_EQ(actual, "last-local");
    }
    EXPECT_EQ(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
    const auto cached_cut = Transaction::epoch_cut();
    ASSERT_NE(cached_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(cached_cut));
    EXPECT_EQ(native_cache_view(0, canonical, "z"), cached_view);
    const auto stale_encoded = mako::Encode("stale-cross-transaction-effect");
    const auto fresh_encoded = mako::Encode("fresh-cross-transaction-effect");
    {
        mako::ShardingOperation operation;
        mako::ShardingRequest unstaged{};
        EXPECT_LT(mako::sharding_batch_request(g_client_tbl->get_table_id(), "z", unstaged), 0);
        EXPECT_LT(mako::sharding_batch_request(
            g_client_tbl->get_table_id(), "capture-only-95", unstaged), 0);
        Sto::start_transaction();
        const auto fresh = mako::sharding_resolve_point(g_client_tbl->get_table_id(), "z", 1);
        mako::ShardingRequest fresh_request{};
        fresh.write_request(fresh_request);
        EXPECT_NE(fresh_request.transaction.sequence, earlier_request.transaction.sequence);
        g_client_tbl->tx_put(nullptr, lcdf::Str("z"), fresh_encoded);
        EXPECT_TRUE(Sto::try_commit_no_paxos());
        EXPECT_NE(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
    }
    EXPECT_EQ(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
    std::string cross_transaction_value;
    EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), cross_transaction_value, std::string::npos));
    EXPECT_EQ(cross_transaction_value, "fresh-cross-transaction-effect");
    {
        mako::ShardingOperation operation;
        Sto::start_transaction();
        g_client_tbl->tx_put(nullptr, lcdf::Str("z"), stale_encoded);
        Sto::silent_abort();
        EXPECT_NE(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
    }
    EXPECT_EQ(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
    EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), cross_transaction_value, std::string::npos));
    EXPECT_EQ(cross_transaction_value, "fresh-cross-transaction-effect");
    EXPECT_EQ(native_cache_view(0, canonical, "z"), cached_view);
    g_client_tbl->put(lcdf::Str("z"), std::string("last-local"));

    // @unsafe - a mode-1 reset is only a placeholder. Binding a real request
    // activates its engine guard; premature finish and a competing identity
    // cannot release it, while terminal cleanup leaves no dummy helper pin.
    TThread::set_mode(1);
    g_db->shard_reset();
    const auto idle_cut = Transaction::epoch_cut();
    EXPECT_NE(idle_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(idle_cut));
    // @unsafe - physical use can also activate an ordinary local operation on
    // the placeholder. Leaving its logical scope cannot finish a live engine.
    uint64_t activated_cut = 0;
    {
        mako::ShardingOperation operation;
        TThread::txn->ensure_epoch();
        activated_cut = Transaction::epoch_cut();
        EXPECT_NE(activated_cut, 0u);
    }
    EXPECT_FALSE(Transaction::epoch_drained(activated_cut));
    Sto::silent_abort();
    EXPECT_TRUE(Transaction::epoch_drained(activated_cut));
    g_db->shard_reset();
    MakoShardRoute helper_route{};
    ASSERT_EQ(mako_sharding_route(canonical, native_bytes(std::string("z")),
                                  &helper_route), MAKO_SHARD_OK);
    mako::ShardingRequest helper_request{};
    helper_request.transaction = {0x45504f4348454c50ULL, 1};
    helper_request.table = canonical;
    helper_request.grant = helper_route.grant;
    helper_request.snapshot_version = helper_route.snapshot_version;
    helper_request.coordinate_length = 1;
    helper_request.coordinate[0] = 'z';
    EXPECT_TRUE(mako::sharding_bind_request(helper_request));
    EXPECT_TRUE(mako::sharding_has_request());
    // @unsafe - each received envelope must govern actual key access, including
    // replacements and a return to the original wire bytes in the same context.
    std::string received_value;
    // STO borrows encoded tuples (including its metadata suffix) through cleanup.
    const std::string fresh_payload = "fresh-point-effect";
    const std::string fresh_effect = mako::Encode(fresh_payload);
    EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                    received_value, std::string::npos));
    EXPECT_EQ(received_value, "last-local");
    auto replacement = helper_request;
    replacement.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
    EXPECT_NE(replacement.snapshot_version, helper_request.snapshot_version);
    mako::sharding_set_request(replacement);
    EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                    received_value, std::string::npos));
    EXPECT_EQ(received_value, "last-local");
    auto forged = replacement;
    ++forged.grant.ownership_version;
    mako::sharding_set_request(forged);
    EXPECT_THROW(mako::sharding_resolve_point(g_client_tbl->get_table_id(), "z", 1),
                 abstract_db::abstract_abort_exception);
    EXPECT_THROW(g_client_tbl->tx_put(nullptr, lcdf::Str("z"), fresh_effect),
                 abstract_db::abstract_abort_exception);
    // Restoring the original envelope must restore access without reviving a
    // forged grant cached by the intervening request.
    mako::sharding_set_request(helper_request);
    EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                    received_value, std::string::npos));
    EXPECT_EQ(received_value, "last-local");
    forged = helper_request;
    forged.coordinate[0] = 'y';
    mako::sharding_set_request(forged);
    EXPECT_THROW(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                     received_value, std::string::npos),
                 abstract_db::abstract_abort_exception);
    mako::sharding_set_request(helper_request);
    EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                    received_value, std::string::npos));
    EXPECT_EQ(received_value, "last-local");
    g_client_tbl->tx_put(nullptr, lcdf::Str("z"), fresh_effect);
    const auto request_cut = Transaction::epoch_cut();
    EXPECT_NE(request_cut, 0u);
    EXPECT_FALSE(Transaction::epoch_drained(request_cut));
    auto competing = helper_request;
    ++competing.transaction.sequence;
    EXPECT_FALSE(mako::sharding_bind_request(competing));
    mako::sharding_finish_request();
    EXPECT_TRUE(mako::sharding_has_request());
    EXPECT_FALSE(Transaction::epoch_drained(request_cut));
    Sto::silent_abort();
    // @unsafe - an abort envelope can arrive after an engine-local abort.
    // Rebinding that same identity must not resurrect the stopped helper or
    // drop the logical pin before terminal request cleanup.
    EXPECT_FALSE(TThread::txn->in_progress());
    EXPECT_FALSE(Sto::in_progress());
    EXPECT_TRUE(mako::sharding_bind_request(helper_request));
    EXPECT_FALSE(TThread::txn->has_epoch());
    EXPECT_FALSE(Transaction::epoch_drained(request_cut));
    mako::sharding_finish_request();
    EXPECT_FALSE(mako::sharding_has_request());
    EXPECT_TRUE(Transaction::epoch_drained(request_cut));
    g_db->shard_reset();
    helper_request.coordinate_length = sizeof(helper_request.coordinate) + 1;
    EXPECT_FALSE(mako::sharding_bind_request(helper_request));
    const auto rejected_cut = Transaction::epoch_cut();
    EXPECT_NE(rejected_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(rejected_cut));
    Sto::silent_abort();
    TThread::set_mode(0);
    std::string receiver_rollback;
    EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), receiver_rollback, std::string::npos));
    EXPECT_EQ(receiver_rollback, "last-local");

    // @unsafe - reopening identical wire bytes is a new protected context.
    // Verify its actual committed effect, not read-your-writes (the legacy
    // point-read kernel does not implement that behavior).
    TThread::set_mode(1);
    g_db->shard_reset();
    helper_request.coordinate_length = 1;
    const auto receiver_reopen_cut = Transaction::epoch_cut();
    EXPECT_NE(receiver_reopen_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(receiver_reopen_cut));
    EXPECT_EQ(native_cache_view(0, canonical, "z"), cached_view);
    ASSERT_TRUE(mako::sharding_bind_request(helper_request));
    EXPECT_TRUE(g_client_tbl->tx_get(nullptr, lcdf::Str("z"),
                                    received_value, std::string::npos));
    EXPECT_EQ(received_value, "last-local");
    g_client_tbl->tx_put(nullptr, lcdf::Str("z"), fresh_effect);
    EXPECT_TRUE(Sto::try_commit_no_paxos());
    mako::sharding_finish_request();
    TThread::set_mode(0);
    std::string receiver_committed;
    EXPECT_TRUE(g_client_tbl->get(lcdf::Str("z"), receiver_committed, std::string::npos));
    EXPECT_EQ(receiver_committed, fresh_payload);
    g_client_tbl->put(lcdf::Str("z"), std::string("last-local"));

    // @unsafe - a real uncommitted Masstree insert makes the middle batch row
    // abort STO. The receiver must roll back earlier rows and never stage later
    // rows on that stopped engine, including when the failed request is retried.
    std::string batch_keys[] = {"batch-before", "batch-conflict", "batch-after"};
    stock::key stock_key;
    stock_key.s_w_id = 1;
    stock_key.s_i_id = 700001;
    item_micro::key micro_key;
    micro_key.i_id = 700001;
    std::string read_keys[] = {EncodeK(stock_key), EncodeK(micro_key)};
    std::string batch_value = "uncommitted";
    std::atomic<unsigned> insertion_state{0};
    auto insertion = rusty::thread::spawn([&] {
        try {
            BenchmarkConfig::setThreadLocalShardIndex(0);
            g_native_runtime->BindToCurrentThread();
            scoped_db_thread_ctx ctx(g_db, true, 0);
            TThread::set_shard_index(0);
            TThread::set_mode(0);
            TThread::in_loading_phase = false;
            Sto::start_transaction();
            g_client_tbl->tx_put(nullptr, lcdf::Str(batch_keys[1]), batch_value);
            for (const auto& key : read_keys)
                g_client_tbl->tx_put(nullptr, lcdf::Str(key), batch_value);
            insertion_state.store(1);
            insertion_state.notify_one();
            insertion_state.wait(1);
            Sto::silent_abort();
        } catch (...) {
            Sto::silent_abort();
            insertion_state.store(3);
            insertion_state.notify_one();
        }
    });
    insertion_state.wait(0);
    EXPECT_EQ(insertion_state.load(), 1u);
    std::exception_ptr batch_failure;
    Transaction::epoch_type batch_cut = 0;
    if (insertion_state.load() == 1) {
        try {
            TThread::set_mode(1);
            g_db->shard_reset();
            mako::ShardReceiver receiver(config_path());
            receiver.Register(g_db, {{g_client_tbl->get_table_id(), g_client_tbl}});
            mako::BatchLockRequestWrapper batch;
            mako::ShardingRequest grant{};
            grant.transaction = {0x424154434841424fULL, 1};
            grant.table = canonical;
            grant.grant = helper_route.grant;
            grant.snapshot_version = helper_route.snapshot_version;
            for (auto& key : batch_keys) {
                grant.coordinate_length = key.size();
                std::memcpy(grant.coordinate, key.data(), key.size());
                batch.add_request(key, batch_value, g_client_tbl->get_table_id(), 0, grant, 0);
            }
            batch.set_req_nr(1);
            batch.get_request_ptr()->sharding = grant;
            batch_cut = Transaction::epoch_cut();
            mako::basic_response_t reply{};
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                EXPECT_EQ(receiver.ReceiveRequest(mako::batchLockReqType,
                    reinterpret_cast<char*>(batch.get_request_ptr()),
                    reinterpret_cast<char*>(&reply)), sizeof(reply));
                EXPECT_EQ(reply.status, mako::ErrorCode::ABORT);
                EXPECT_FALSE(mako::sharding_has_request());
                EXPECT_FALSE(TThread::txn->has_epoch());
            }
            mako::basic_request_t abort{};
            abort.req_nr = 2;
            abort.sharding = grant;
            EXPECT_EQ(receiver.ReceiveRequest(mako::abortReqType,
                reinterpret_cast<char*>(&abort), reinterpret_cast<char*>(&reply)), sizeof(reply));
            EXPECT_EQ(reply.status, mako::ErrorCode::SUCCESS);

            // @unsafe - both multi-row read variants must stop at a real
            // engine-aborting read and leave the next ordinary request usable.
            for (unsigned variant = 0; variant < 2; ++variant) {
                const auto& key = read_keys[variant];
                ++grant.transaction.sequence;
                grant.coordinate_length = key.size();
                std::memcpy(grant.coordinate, key.data(), key.size());
                EXPECT_TRUE(mako::sharding_bind_request(grant));
                mako::get_request_t read{};
                read.req_nr = 1;
                read.sharding = grant;
                read.table_id = g_client_tbl->get_table_id();
                read.len = key.size();
                std::memcpy(read.key, key.data(), key.size());
                mako::get_response_t read_reply;
                size_t length = 0;
                if (variant == 0) {
                    receiver.HandleGetMegaRequest(reinterpret_cast<char*>(&read),
                        reinterpret_cast<char*>(&read_reply), length);
                } else {
                    receiver.HandleGetMicroMegaRequest(reinterpret_cast<char*>(&read),
                        reinterpret_cast<char*>(&read_reply), length);
                }
                EXPECT_EQ(read_reply.status, mako::ErrorCode::ABORT);
                EXPECT_FALSE(TThread::txn->in_progress());
                mako::sharding_finish_request();
                g_db->shard_reset();
                ++grant.transaction.sequence;
                grant.coordinate_length = 1;
                grant.coordinate[0] = 'z';
                read.sharding = grant;
                read.len = 1;
                read.key[0] = 'z';
                receiver.ReceiveRequest(mako::getReqType,
                    reinterpret_cast<char*>(&read), reinterpret_cast<char*>(&read_reply));
                EXPECT_EQ(read_reply.status, mako::ErrorCode::SUCCESS);
                abort.sharding = grant;
                receiver.ReceiveRequest(mako::abortReqType,
                    reinterpret_cast<char*>(&abort), reinterpret_cast<char*>(&reply));
                EXPECT_EQ(reply.status, mako::ErrorCode::SUCCESS);
                EXPECT_FALSE(mako::sharding_has_request());
            }
        } catch (...) {
            batch_failure = std::current_exception();
        }
        Sto::silent_abort();
        mako::sharding_finish_request();
        TThread::set_mode(0);
    }
    insertion_state.store(2);
    insertion_state.notify_one();
    insertion.join().unwrap();
    if (batch_failure) std::rethrow_exception(batch_failure);
    EXPECT_NE(batch_cut, 0u);
    EXPECT_TRUE(Transaction::epoch_drained(batch_cut));
    for (const auto& key : batch_keys) {
        std::string value;
        EXPECT_FALSE(g_client_tbl->get(lcdf::Str(key), value, std::string::npos));
    }
    for (const auto& key : read_keys) {
        std::string value;
        EXPECT_FALSE(g_client_tbl->get(lcdf::Str(key), value, std::string::npos));
    }

    // @unsafe - exercise canonical receiver dispatch, not the mapping helper in
    // isolation. Each call terminates a distinct transaction on the same worker,
    // retaining only reusable physical addresses between calls.
    mako::ShardReceiver cached_receiver(config_path());
    cached_receiver.Register(g_db, {
        {g_client_tbl->get_table_id(), g_client_tbl},
        {g_server_tbl->get_table_id(), g_server_tbl}});
    uint64_t receiver_sequence = 0;
    auto receive_cached = [&](unsigned owner, mako::ShardingRequest request,
                              const std::string& key, bool admitted,
                              const std::string& expected_value) {
        BenchmarkConfig::setThreadLocalShardIndex(owner);
        TThread::set_shard_index(owner);
        TThread::set_mode(1);
        g_db->shard_reset();
        request.transaction = {UINT64_C(0x4341434845525043), ++receiver_sequence};
        mako::get_request_t get{};
        get.req_nr = 1;
        get.table_id = g_client_tbl->get_table_id();
        get.sharding = request;
        get.len = key.size();
        std::memcpy(get.key, key.data(), key.size());
        mako::get_response_t reply{};
        cached_receiver.ReceiveRequest(mako::getReqType,
            reinterpret_cast<char*>(&get), reinterpret_cast<char*>(&reply));
        if (admitted) {
            EXPECT_EQ(reply.status, mako::ErrorCode::SUCCESS);
        } else {
            EXPECT_TRUE(reply.status == mako::ErrorCode::ABORT
                        || reply.status == mako::ErrorCode::ERROR);
        }
        if (admitted && reply.status == mako::ErrorCode::SUCCESS) {
            EXPECT_GE(reply.len, expected_value.size());
            if (reply.len >= expected_value.size())
                EXPECT_EQ(std::string(reply.value, expected_value.size()), expected_value);
        }
        // Use identity-only terminal traffic even when the data envelope was
        // malformed: an invalid coordinate must not prevent helper cleanup.
        mako::basic_request_t abort{};
        abort.req_nr = 2;
        abort.sharding.transaction = request.transaction;
        mako::basic_response_t terminal{};
        cached_receiver.ReceiveRequest(mako::abortReqType,
            reinterpret_cast<char*>(&abort), reinterpret_cast<char*>(&terminal));
        EXPECT_EQ(terminal.status, mako::ErrorCode::SUCCESS);
        Sto::silent_abort();
        mako::sharding_finish_request();
        EXPECT_FALSE(mako::sharding_has_request());
        EXPECT_EQ(Transaction::tinfo[TThread::id()].epoch.load(), 0u);
        TThread::set_mode(0);
        BenchmarkConfig::setThreadLocalShardIndex(0);
        TThread::set_shard_index(0);
    };
    auto unmoved_request = earlier_request;
    auto migrating_request = earlier_request;
    const std::string migrating_key = "m-cache-binding";
    const std::string migrating_value = "receiver-cache-middle";
    ASSERT_TRUE(g_client_tbl->put(lcdf::Str(migrating_key), migrating_value));
    migrating_request.coordinate_length = migrating_key.size();
    std::memcpy(migrating_request.coordinate, migrating_key.data(), migrating_key.size());
    MakoShardRoute before_migration{};
    ASSERT_EQ(mako_sharding_route(canonical, native_bytes(migrating_key),
                                  &before_migration), MAKO_SHARD_OK);
    migrating_request.grant = before_migration.grant;
    migrating_request.snapshot_version = before_migration.snapshot_version;
    for (unsigned round = 0; round < 3; ++round) {
        receive_cached(0, unmoved_request, "z", true, "last-local");
        receive_cached(0, migrating_request, migrating_key, true, migrating_value);
        auto malformed = migrating_request;
        if (round == 0) malformed.coordinate_length = sizeof(malformed.coordinate) + 1;
        if (round == 1) malformed.grant.owner = 1;
        if (round == 2) malformed.coordinate[0] = 'z';
        receive_cached(0, malformed, migrating_key, false, "");
        receive_cached(0, unmoved_request, "z", true, "last-local");
    }

    const MakoShardTxn nonce{0x4d49584544534341ULL, 1};
    MakoShardAdminResult result{};
    ASSERT_EQ(mako_sharding_admin_begin(nonce, native_bytes(name), native_bytes(lo),
        1, native_bytes(hi), 0, 1, native.hosts[0].get(), native_admin_call, &result), MAKO_SHARD_OK);
    for (unsigned attempt = 0; attempt < 900 && result.outcome == 0; ++attempt) {
        usleep(100000);
        ASSERT_EQ(mako_sharding_admin_poll(nonce, native.hosts[0].get(), native_admin_call,
            &result), MAKO_SHARD_OK);
    }
    ASSERT_EQ(result.outcome, 1u);
    MakoShardRoute middle{};
    bool routed = false;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (mako_sharding_route(canonical, native_bytes(lo), &middle) == MAKO_SHARD_OK
                && middle.grant.owner == 1) { routed = true; break; }
        usleep(50000);
    }
    ASSERT_TRUE(routed);
    // The very same physical binding now contains both an unaffected range and
    // a migrated range with a different grant. Old successful receiver lookups
    // must neither authorize the old middle nor redirect the unaffected row.
    auto current_middle = migrating_request;
    current_middle.grant = middle.grant;
    current_middle.snapshot_version = middle.snapshot_version;
    for (unsigned round = 0; round < 2; ++round) {
        receive_cached(0, unmoved_request, "z", true, "last-local");
        receive_cached(0, migrating_request, migrating_key, false, "");
        receive_cached(0, unmoved_request, "z", true, "last-local");
        receive_cached(1, current_middle, migrating_key, true, migrating_value);
        auto borrowed_grant = unmoved_request;
        borrowed_grant.grant = middle.grant;
        borrowed_grant.snapshot_version = middle.snapshot_version;
        receive_cached(1, borrowed_grant, "z", false, "");
        receive_cached(1, current_middle, migrating_key, true, migrating_value);
    }
    BenchmarkConfig::setThreadLocalShardIndex(1);
    TThread::set_shard_index(1);
    EXPECT_TRUE(g_server_tbl->remove(lcdf::Str(migrating_key)));
    BenchmarkConfig::setThreadLocalShardIndex(0);
    TThread::set_shard_index(0);

    // @unsafe - reuse the existing real transport/helper bootstrap and map ABI.
    auto transport = rusty::thread::spawn([]() { rpc_server_thread("localhost", g_config); });
    transport.detach();
    sleep(2);
    std::map<int, abstract_ordered_index*> tables{{g_server_tbl->get_table_id(), g_server_tbl}};
    auto helper = rusty::thread::spawn([tables = std::move(tables)]() mutable {
        helper_server_thread(g_config, g_db, std::move(tables));
    });
    helper.detach();
    sleep(1);
    auto client = rusty::make_box<mako::ShardClient>(g_config->configFile, "localhost", 0, 0);
    TThread::sclient = client.get();
    TThread::set_mode(0);
    TThread::set_shard_index(0);
    TThread::set_pid(0);
    TThread::set_nshards(2);
    struct ResetClient {
        // @unsafe - clear TLS before client destruction, including ASSERT exits.
        ~ResetClient() { TThread::sclient = nullptr; }
    } reset_client;
    for (bool reverse : {false, true}) {
        str_arena arena;
        NativeMixedRows complete;
        complete.retain = true;
        if (reverse) g_client_tbl->rscan(high, nullptr, complete, &arena);
        else g_client_tbl->scan(empty, nullptr, complete, &arena);
        ASSERT_EQ(complete.rows.size(), expected.rows.size());
        for (size_t i = 0; i < complete.rows.size(); ++i) {
            const auto at = reverse ? expected.rows.size() - i - 1 : i;
            EXPECT_EQ(complete.rows[i], expected.rows[at]);
        }
        NativeMixedRows churn;
        if (reverse) g_client_tbl->rscan(high, nullptr, churn, nullptr);
        else g_client_tbl->scan(empty, nullptr, churn, nullptr);
        for (size_t i = 0; i < complete.values.size(); ++i)
            EXPECT_EQ(*complete.values[i], complete.rows[i].second);
        // Stop/throw in first local, first remote, and final local segments.
        for (size_t stop : {size_t(1), reverse ? size_t(3) : size_t(4),
                           reverse ? size_t(43) : size_t(44)}) {
            for (bool fail : {false, true}) {
                NativeMixedRows shortened;
                shortened.stop = stop;
                shortened.fail = fail;
                bool caught = false;
                try {
                    if (reverse) g_client_tbl->rscan(high, nullptr, shortened, nullptr);
                    else g_client_tbl->scan(empty, nullptr, shortened, nullptr);
                } catch (const NativeMixedCallbackFailure&) { caught = true; }
                ASSERT_EQ(caught, fail);
                ASSERT_EQ(shortened.rows.size(), stop);
                for (size_t i = 0; i < stop; ++i)
                    EXPECT_EQ(shortened.rows[i], expected.rows[reverse ? expected.rows.size() - i - 1 : i]);
                ASSERT_FALSE(TThread::txn && TThread::txn->in_progress());
            }
        }
    }
    // Boundary keys adjoining NUL are exact, not wire continuation sentinels.
    NativeMixedRows bounded, reverse_bounded, empty_only;
    g_client_tbl->scan(binary, &successor, bounded, nullptr);
    g_client_tbl->rscan(successor, &binary, reverse_bounded, nullptr);
    g_client_tbl->rscan(empty, nullptr, empty_only, nullptr);
    ASSERT_EQ(bounded.rows.size(), 1u);
    EXPECT_EQ(bounded.rows[0], expected.rows[1]);
    ASSERT_EQ(reverse_bounded.rows.size(), 1u);
    EXPECT_EQ(reverse_bounded.rows[0], expected.rows[2]);
    ASSERT_EQ(empty_only.rows.size(), 1u);
    EXPECT_EQ(empty_only.rows[0], expected.rows[0]);
}

}  // namespace
