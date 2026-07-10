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
#include "storage/mbta_wrapper.hh"
#include "storage/mbta_sharded_ordered_index.hh"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/shardClient.h"
#include "rocks_interface/client_tcp_server.h"
#include "rocks_interface/local_table.hh"
#include "rocks_interface/remote_db.hh"
#include "sto/Transaction.hh"
#include "sto/sync_util.hh"

#include <gtest/gtest.h>

import std;
import cluster;   // janus::get_migration_guard() -- the server's migration freeze registry

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
        g_client_tbl = mbta_index_build("nontxn_dist", kRemoteTableId,
                                        /*is_remote=*/true);
        g_server_tbl = mbta_index_build("nontxn_dist", kRemoteTableId,
                                        /*is_remote=*/false);

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
    ASSERT_TRUE(mako::RemoteDB::ConnectNontxn("127.0.0.1", 31307, &rdb).ok());
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

    rdb->Disconnect();
    delete rdb;
    tcp->Stop();
}

// ---------------------------------------------------------------------------
// Migration freeze enforcement over the REAL non-txn RPC path: while a range is
// frozen (as a migration's source shard would freeze it), a client's remote
// write to a key in that range is rejected by the server's RunNontxnOp
// (SERVER_BUSY) and does NOT land; keys outside the range are unaffected; and
// once unfrozen the write goes through. Server and client share the
// process-global MigrationGuard here (one process), so freezing it directly
// stands in for the FreezeRange RPC the coordinator would issue on the source.
// ---------------------------------------------------------------------------
// nontxnPut returns the raw server status (the higher-level put() wrapper is
// what RETRIES on SERVER_BUSY -- which is exactly the freeze contract: a client
// retries until the cutover reroutes it. We call nontxnPut directly so the test
// observes the rejection deterministically instead of retrying forever.
TEST_F(MakoNontxnDistributed, FrozenRangeReturnsServerBusyOnRemoteWrite) {
    auto& guard = janus::get_migration_guard();
    guard.clear();
    bool op = false;

    // Baseline: nothing frozen -> the remote write succeeds and lands.
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "fz_ok", "v", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    { std::string out; ASSERT_TRUE(g_server_tbl->get(lcdf::Str("fz_ok"), out, std::string::npos));
      EXPECT_EQ(out, "v"); }

    // Freeze [fz_m, fz_t) on the server shard (stands in for the FreezeRange RPC
    // the coordinator issues on the source; server + client share the guard here).
    guard.freeze("", "fz_m", "fz_t");

    // A remote write INSIDE the frozen range is rejected by the server's
    // RunNontxnOp with SERVER_BUSY, and does NOT land on the server view.
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "fz_m5", "frozen", &op),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY));
    { std::string out; EXPECT_FALSE(g_server_tbl->get(lcdf::Str("fz_m5"), out, std::string::npos))
        << "a write to a frozen range must not land on the server"; }

    // A remote write OUTSIDE the frozen range still succeeds.
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "fz_zzz", "outside", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));

    // Unfreeze -> the previously-frozen key now writes through.
    guard.unfreeze("", "fz_m", "fz_t");
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "fz_m5", "after-thaw", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    { std::string out; ASSERT_TRUE(g_server_tbl->get(lcdf::Str("fz_m5"), out, std::string::npos));
      EXPECT_EQ(out, "after-thaw"); }

    guard.clear();
}

// ---------------------------------------------------------------------------
// The MOVED state over the REAL RPC path: while a range is merely FROZEN
// (mid-migration), reads keep serving from this shard; once the shard SHEDS the
// range (mark_moved, as DropRange does at commit), READS are rejected too --
// a stale-routed client must never see a clean miss for data that lives on the
// new owner. Unfreeze (ownership regained) restores both.
// ---------------------------------------------------------------------------
TEST_F(MakoNontxnDistributed, MovedRangeRejectsReadsAndWritesUntilUnfenced) {
    auto& guard = janus::get_migration_guard();
    guard.clear();
    bool op = false;
    std::string out;

    // Seed a row the reads will target.
    ASSERT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "mv_m5", "v0", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));

    // FROZEN: writes rejected, reads still served (the mid-migration contract).
    guard.freeze("", "mv_m", "mv_t");
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "mv_m5", "x", &op),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY));
    EXPECT_EQ(TThread::sclient->nontxnGet(kRemoteTableId, "mv_m5", out),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    EXPECT_EQ(out, "v0");

    // MOVED (the shard shed the range): reads join writes behind the fence.
    guard.mark_moved("", "mv_m", "mv_t");
    EXPECT_EQ(TThread::sclient->nontxnGet(kRemoteTableId, "mv_m5", out),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY))
        << "a stale-routed read of a moved range must retry, not see a miss";
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "mv_m5", "x", &op),
              static_cast<int>(mako::ErrorCode::SERVER_BUSY));
    // Outside the range: unaffected.
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "mv_zz", "ok", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));

    // Ownership regained (what the master's commit does on the destination):
    // the exact-triple unfence clears the moved entry; both ops serve again.
    guard.unfreeze("", "mv_m", "mv_t");
    EXPECT_EQ(TThread::sclient->nontxnGet(kRemoteTableId, "mv_m5", out),
              static_cast<int>(mako::ErrorCode::SUCCESS));
    EXPECT_EQ(out, "v0");
    EXPECT_EQ(TThread::sclient->nontxnPut(kRemoteTableId, "mv_m5", "v1", &op),
              static_cast<int>(mako::ErrorCode::SUCCESS));

    guard.clear();
}

}  // namespace
