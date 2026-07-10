// Distributed shard migration over RPC: the ShardMigrator coordinator drives a
// migration whose SOURCE participant lives in another "process", reached via the
// ShardDataService RPC (a RemoteShardData proxy) over a real rrr server+client
// loopback in one process. Proves the cross-process online migration reuses the
// SAME coordinator + 2PC protocol (copy -> checksum-verify -> commit) with the
// range flowing over the wire.
//
// The ShardData BACKEND here is an in-memory std::map (MapShardData) rather than
// the real mbta engine -- exactly as src/deptran/config_kv_service.h's test uses
// an InMemoryKvStore. The backend is orthogonal to the transport: mbta as a
// ShardData is proven by tests/shard_migration_mbta_test.cc (Stage 2, local
// migration on real mbta); this test proves the DISTRIBUTED path over a real
// socket. Keeping mbta_wrapper.hh out of this TU also avoids a toolchain tarpit
// (mbta's textual-STL + `import std;` in one test TU compiles pathologically).
//
// TU footprint: rcc_rpc.h (via shard_data_service.h) + gtest -- the known-good
// config_kv_service_test.cc profile. The rrr::Server/Client wiring is isolated
// in shard_data_rpc_harness.cc (see that file for why).

#include "shard_data_rpc_harness.h" // rrr server/client wiring (rrr.hpp in its .cc)
#include "shard_data_service.h"     // ShardDataServiceImpl / RemoteShardData (+ rcc_rpc.h; import cluster: ShardMaster)

#include <gtest/gtest.h>

#include <map>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

// In-memory ShardData backend (std::map keeps keys sorted -> scan_range returns
// ascending). checksum / verify_range / drop_range / copy_range_from are
// inherited from ShardData, defined over these primitives.
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
    std::vector<KvPair> scan_range(const std::string& lo, const std::string& hi) override {
        std::vector<KvPair> o;
        for (auto& kv : m_) if (kv.first >= lo && kv.first < hi) o.emplace_back(kv.first, kv.second);
        return o;
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo, const std::string& hi,
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

std::string mkkey(int i) { char b[16]; snprintf(b, sizeof b, "k%05d", i); return b; }
std::string mkval(int i) { return "v" + std::to_string(i); }

// A seeded in-memory shard (leaked; process-lifetime, so it can back an rrr
// server for the whole test).
janus::ShardData* make_shard(int count) {
    auto* s = new MapShardData();
    for (int i = 0; i < count; i++) s->put(mkkey(i), mkval(i));
    return s;
}

// ---- server-logic (Do*) directly, no socket ----
// Every op names its table; this single-table service (SingleTableCatalog
// inside) resolves ANY name to the one wrapped shard, so "t" is arbitrary.
TEST(ShardMigrationRpc, ServiceHandlerLogic) {
    janus::ShardData* shard = make_shard(20);
    janus::ShardDataServiceImpl svc(shard);
    const std::string t = "t";

    auto rows = svc.DoScanRange(t, mkkey(5), mkkey(10), 100);   // [k00005,k00010): 5 keys
    EXPECT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[mkkey(7)], mkval(7));

    const uint64_t ck = svc.DoChecksum(t, mkkey(5), mkkey(10));
    EXPECT_EQ(svc.DoChecksum(t, mkkey(5), mkkey(10)), ck);         // stable
    EXPECT_TRUE(svc.DoVerifyRange(t, mkkey(5), mkkey(10), ck));    // matches self
    EXPECT_FALSE(svc.DoVerifyRange(t, mkkey(5), mkkey(10), ck ^ 1));

    svc.DoPut(t, "nk", "nv");
    std::string out; EXPECT_TRUE(svc.DoGet(t, "nk", &out)); EXPECT_EQ(out, "nv");
    svc.DoRemove(t, "nk"); EXPECT_FALSE(svc.DoGet(t, "nk", &out));

    svc.DoDropRange(t, mkkey(5), mkkey(10));
    EXPECT_EQ(svc.DoScanRange(t, mkkey(5), mkkey(10), 100).size(), 0u);
}

// ---- the real thing: a cross-process migration over an rrr loopback ----
TEST(ShardMigrationRpc, DistributedMigrationOverRpc) {
    // The SOURCE shard lives "remotely": behind an rrr ShardDataService server.
    janus::ShardData* src_shard = make_shard(200);

    const std::string addr = "127.0.0.1:31899";
    ASSERT_EQ(rpc_harness::start_server(src_shard, addr), 0) << "server bind failed on " << addr;
    janus::ShardDataServiceProxy* proxy = rpc_harness::connect_client(addr);
    ASSERT_NE(proxy, nullptr) << "client connect failed to " << addr;

    janus::RemoteShardData remote_src(proxy);   // the source, reached over the wire

    // Round-trip sanity through the proxy.
    { std::string out; ASSERT_TRUE(remote_src.get(mkkey(3), out)); EXPECT_EQ(out, mkval(3)); }
    EXPECT_EQ(remote_src.scan_range_limited(mkkey(0), mkkey(9999), 50).size(), 50u);

    // The DESTINATION shard is local (coordinator co-located with the new shard).
    MapShardData local_dst;

    const std::string lo = mkkey(50), hi = mkkey(150);   // [50,150): 100 keys

    // Drive the SAME coordinator -- the cluster ShardMaster -- but with a REMOTE
    // source participant (shard 0, over the wire) and a local destination (shard
    // 1). The master's ConfigManager is an isolated in-memory store here.
    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    janus::ClusterConfig cfg = janus::ClusterConfig::new_();
    janus::ShardMaster master = janus::ShardMaster::new_(&cm, &cfg);
    ASSERT_EQ(master.register_shard({"remote_src"}, &remote_src), 0u);
    ASSERT_EQ(master.register_shard({"local_dst"}, &local_dst), 1u);

    ASSERT_TRUE(master.begin_migration(/*source=*/0, /*dest=*/1, /*table=*/"", lo, hi));
    master.background_copy();  // local_dst pulls the range from remote_src (ScanRange RPCs)
    master.lock_range();
    master.final_sync();       // remote_src.checksum() [RPC] vs local_dst.verify_range()
    ASSERT_TRUE(master.both_prepared());
    ASSERT_TRUE(master.commit_migration());  // remote_src.drop_range() [DropRange RPC]

    // The range now lives on the local destination...
    EXPECT_EQ(local_dst.range_count(lo, hi), 100u);
    { std::string out; ASSERT_TRUE(local_dst.get(mkkey(99), out)); EXPECT_EQ(out, mkval(99)); }
    // ...and the remote source shed it (checked over the wire).
    EXPECT_EQ(remote_src.scan_range(lo, hi).size(), 0u);
    // Neighbors on the source are untouched.
    EXPECT_EQ(remote_src.scan_range(mkkey(0),   mkkey(50)).size(),  50u);
    EXPECT_EQ(remote_src.scan_range(mkkey(150), mkkey(200)).size(), 50u);
}

// The FreezeRange / UnfreezeRange RPCs set and clear the source shard's
// MigrationGuard over the wire -- so the shard's non-txn write handler will
// reject frozen keys during the migration. Server and test share the
// process-global guard in this loopback, so we can inspect it directly.
TEST(ShardMigrationRpc, FreezeRangeOverRpcSetsAndClearsTheGuard) {
    janus::get_migration_guard().clear();
    janus::ShardData* shard = make_shard(20);

    const std::string addr = "127.0.0.1:31901";
    ASSERT_EQ(rpc_harness::start_server(shard, addr), 0) << "server bind failed on " << addr;
    janus::ShardDataServiceProxy* proxy = rpc_harness::connect_client(addr);
    ASSERT_NE(proxy, nullptr) << "client connect failed to " << addr;
    janus::RemoteShardData remote(proxy);

    EXPECT_FALSE(janus::get_migration_guard().is_frozen("", mkkey(7)));
    remote.freeze_range(mkkey(5), mkkey(10));                             // FreezeRange RPC
    EXPECT_TRUE(janus::get_migration_guard().is_frozen("", mkkey(7)));    // inside [5,10)
    EXPECT_FALSE(janus::get_migration_guard().is_frozen("", mkkey(3)));   // outside
    EXPECT_FALSE(janus::get_migration_guard().is_frozen("", mkkey(10)));  // hi exclusive
    remote.unfreeze_range(mkkey(5), mkkey(10));                           // UnfreezeRange RPC
    EXPECT_FALSE(janus::get_migration_guard().is_frozen("", mkkey(7)));
}

// ---- destination-driven copy: rows flow source -> dest, never via the master ----
// BOTH participants are remote (two rrr servers); the coordinator's
// background_copy delegates to the DESTINATION with one PullRange control RPC,
// and the destination pulls the range directly from the source's service. The
// destination service's pull counter proves the copy ran dest-side (under the
// old master-relayed shape it would stay 0 and the master would ship every row
// itself as per-key PutKey RPCs).
TEST(ShardMigrationRpc, DestinationDrivenCopyBypassesTheMaster) {
    janus::ShardData* src_backing = make_shard(200);
    auto* dst_backing = new MapShardData();

    const std::string src_addr = "127.0.0.1:31897";
    const std::string dst_addr = "127.0.0.1:31898";
    ASSERT_EQ(rpc_harness::start_server(src_backing, src_addr), 0);
    janus::ShardDataServiceImpl* dst_svc =
        rpc_harness::start_server_impl(dst_backing, dst_addr);
    ASSERT_NE(dst_svc, nullptr);

    janus::ShardDataServiceProxy* src_proxy = rpc_harness::connect_client(src_addr);
    janus::ShardDataServiceProxy* dst_proxy = rpc_harness::connect_client(dst_addr);
    ASSERT_NE(src_proxy, nullptr);
    ASSERT_NE(dst_proxy, nullptr);

    // Address-bound remote participants: the source can IDENTIFY itself as a
    // pull endpoint, the destination can be delegated to.
    janus::RemoteShardData remote_src(src_proxy, "t", src_addr);
    janus::RemoteShardData remote_dst(dst_proxy, "t", dst_addr);

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    janus::ClusterConfig cfg = janus::ClusterConfig::new_();
    janus::ShardMaster master = janus::ShardMaster::new_(&cm, &cfg);
    ASSERT_EQ(master.register_shard({"remote_src"}, &remote_src), 0u);
    ASSERT_EQ(master.register_shard({"remote_dst"}, &remote_dst), 1u);

    const std::string lo = mkkey(50), hi = mkkey(150);
    ASSERT_TRUE(master.begin_migration(0, 1, "", lo, hi));
    master.background_copy();   // ONE PullRange to the dest; rows go src -> dst direct
    master.lock_range();
    master.final_sync();
    ASSERT_TRUE(master.both_prepared());
    ASSERT_TRUE(master.commit_migration());

    EXPECT_EQ(dst_svc->pull_range_calls, 1)
        << "the DESTINATION must have executed the pull (not the master)";
    EXPECT_EQ(dst_backing->scan_range(lo, hi).size(), 100u);   // rows landed dest-side
    EXPECT_EQ(src_backing->scan_range(lo, hi).size(), 0u);     // source shed the range
    { std::string v; ASSERT_TRUE(dst_backing->get(mkkey(99), v)); EXPECT_EQ(v, mkval(99)); }
}

}  // namespace
