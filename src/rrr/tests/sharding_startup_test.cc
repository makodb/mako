/**
 * @file sharding_startup_test.cc
 * @brief Tests for sharding policy startup flow integration.
 *
 * Tests verify that:
 * 1. C-node loads sharding policy from RocksDB on reboot
 * 2. Data nodes fetch sharding policy from c-node via RPC
 * 3. Initializer nodes send sharding policy to c-node
 * 4. End-to-end startup flow works correctly
 */

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include "../rrr.hpp"
#include "../src/deptran/config_store.h"
#include "../src/deptran/config_service.h"
#include "../src/deptran/sharding_policy.h"
#include "../src/deptran/sharding_policy_builder.h"
#include "../src/deptran/sharding_policy_cache.h"
#include "../src/deptran/config_client.h"
#include "../src/mako/benchmarks/tpcc_sharding.h"

import std;

namespace janus {
namespace test {

namespace fs = std::filesystem;
using namespace rrr;

// @safe - Atomic counter for port allocation
static std::atomic<int> g_startup_test_port{0};
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definitions.
#if RUSTYCPP_RUST
const PORT_BASE: i32 = 22000;
const PORT_RANGE: i32 = 10000;
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_startup_test.1 version=1 rust_sha256=a5a91d4fd47eb78cd72342a2286e702c8f7b0dd95f40ab8c48c64aeed8d0d9a3*/
constexpr int32_t PORT_BASE = static_cast<int32_t>(22000);
constexpr int32_t PORT_RANGE = static_cast<int32_t>(10000);
/*RUSTYCPP:GEN-END id=sharding_startup_test.1*/

// @safe - Atomic counter for unique database paths
static std::atomic<int> g_db_path_counter{0};

// =============================================================================
// Test Fixture
// =============================================================================

// @unsafe - Uses file I/O and RPC
class ShardingStartupTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    std::string test_db_path_;
    int base_port_;

    // @safe - Allocate unique port range for this fixture
    ShardingStartupTest()
        : base_port_(PORT_BASE + (g_startup_test_port.fetch_add(100) % PORT_RANGE)) {}

    void SetUp() override {
        // @unsafe { file I/O }
        poll_thread_ = rusty::Some(PollThread::create());

        // Create unique temp directory for test database
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir == nullptr) {
            tmpdir = "/tmp";
        }
        int db_id = g_db_path_counter.fetch_add(1);
        test_db_path_ = std::string(tmpdir) + "/sharding_startup_test_";
        test_db_path_ += std::to_string(::testing::UnitTest::GetInstance()->random_seed());
        test_db_path_ += "_" + std::to_string(db_id);

        // @unsafe { file I/O }
        fs::remove_all(test_db_path_);

        // Clear sharding policy cache
        get_sharding_policy_cache().clear();
    }

    void TearDown() override {
        // Shutdown poll thread
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
        }

        // @unsafe { file I/O }
        fs::remove_all(test_db_path_);

        // Clear sharding policy cache
        get_sharding_policy_cache().clear();
    }

    // @safe - Allocate unique port within this fixture's range
    int next_port() {
        static std::atomic<int> offset{0};
        return base_port_ + offset.fetch_add(1);
    }

    // @safe - Create a sample sharding policy
    ShardingPolicySet create_sample_policy(int num_warehouses = 10, int num_shards = 2) {
        return create_tpcc_sharding_policy(num_warehouses, num_shards);
    }

    // @unsafe - Create c-node server with ConfigService
    struct CNodeServer {
        ConfigStore* store;
        ConfigServiceImpl* service;
        Server* rpc_server;
        int port;

        ~CNodeServer() {
            if (rpc_server) {
                rpc_server->graceful_shutdown(100);  // 100ms timeout for tests
                delete rpc_server;
            }
            if (service) {
                delete service;
            }
            if (store) {
                store->close();
                delete store;
            }
        }
    };

    // @unsafe - Creates and starts c-node server
    rusty::Option<rusty::Box<CNodeServer>> create_cnode_server(int port) {
        auto cnode = rusty::make_box<CNodeServer>();
        cnode->port = port;

        // Create and open store
        cnode->store = new ConfigStore(test_db_path_);
        if (!cnode->store->open()) {
            return rusty::None;
        }

        // Create service
        cnode->service = new ConfigServiceImpl(*cnode->store);

        // Create RPC server
        cnode->rpc_server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
        auto service_box = rusty::make_box<ConfigServiceImpl>(*cnode->store);
        cnode->rpc_server->reg_service_typed(std::move(service_box));

        std::string addr = "0.0.0.0:" + std::to_string(port);
        if (cnode->rpc_server->start(reinterpret_cast<const int8_t*>(addr.c_str())) != 0) {
            return rusty::None;
        }

        return cnode;
    }
};

// =============================================================================
// C-Node First Boot Tests
// =============================================================================

// @unsafe - RocksDB I/O
TEST_F(ShardingStartupTest, CNodeFirstBootNoStoredPolicy) {
    // Create store without any stored policy
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Verify no sharding policy exists
    EXPECT_FALSE(store.has_sharding_policy());
    EXPECT_EQ(0u, store.get_sharding_policy_version());

    store.close();
}

// @unsafe - RocksDB I/O
TEST_F(ShardingStartupTest, CNodeRebootLoadsStoredPolicy) {
    // First boot: save policy
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        auto policy = create_sample_policy();
        EXPECT_TRUE(store.save_sharding_policy(policy));

        store.close();
    }

    // Reboot: verify policy is loaded
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        EXPECT_TRUE(store.has_sharding_policy());
        auto loaded = store.load_sharding_policy();
        EXPECT_TRUE(loaded.is_some());

        auto policy = loaded.unwrap();
        EXPECT_EQ(2u, policy.num_shards);  // create_sample_policy uses 10 warehouses, 2 shards
        EXPECT_GE(policy.policies.size(), 9u);  // At least 9 TPC-C tables

        store.close();
    }
}

// @unsafe - RocksDB I/O
TEST_F(ShardingStartupTest, CNodeRebootPolicyVersionPreserved) {
    // First boot: save policy with specific version
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        auto policy = create_sample_policy();
        policy.version = 12345;
        EXPECT_TRUE(store.save_sharding_policy(policy));

        store.close();
    }

    // Reboot: verify version is preserved
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        EXPECT_EQ(12345u, store.get_sharding_policy_version());

        store.close();
    }
}

// =============================================================================
// C-Node RPC Tests
// =============================================================================

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, CNodeServesShardingPolicyViaRpc) {
    int port = next_port();

    // Start c-node server with policy
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    // Store a policy
    auto policy = create_sample_policy();
    EXPECT_TRUE(cnode.as_ref().unwrap()->store->save_sharding_policy(policy));

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create client and fetch policy
    std::string addr = "127.0.0.1:" + std::to_string(port);
    ConfigClient client(addr);
    EXPECT_TRUE(client.connect());

    auto has_policy = client.has_sharding_policy();
    EXPECT_TRUE(has_policy.is_some());
    EXPECT_TRUE(has_policy.unwrap());

    auto fetched = client.fetch_sharding_policy();
    EXPECT_TRUE(fetched.is_some());

    auto fetched_policy = fetched.unwrap();
    EXPECT_EQ(policy.num_shards, fetched_policy.num_shards);

    client.disconnect();
}

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, CNodeServesNoPolicyWhenEmpty) {
    int port = next_port();

    // Start c-node server without any policy
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create client and check
    std::string addr = "127.0.0.1:" + std::to_string(port);
    ConfigClient client(addr);
    EXPECT_TRUE(client.connect());

    auto has_policy = client.has_sharding_policy();
    EXPECT_TRUE(has_policy.is_some());
    EXPECT_FALSE(has_policy.unwrap());
    auto version = client.fetch_sharding_version();
    EXPECT_TRUE(!version.is_some() || version.unwrap() == 0u);

    client.disconnect();
}

// =============================================================================
// Initializer Node Tests
// =============================================================================

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, InitializerSendsPolicyToCNode) {
    int port = next_port();

    // Start c-node server
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify no policy initially
    EXPECT_FALSE(cnode.as_ref().unwrap()->store->has_sharding_policy());

    // Send TPC-C policy from initializer
    std::string addr = "127.0.0.1:" + std::to_string(port);
    EXPECT_TRUE(mako::send_tpcc_sharding_policy_to_cnode(addr, 10, 2));

    // Verify policy is stored
    EXPECT_TRUE(cnode.as_ref().unwrap()->store->has_sharding_policy());

    auto stored = cnode.as_ref().unwrap()->store->load_sharding_policy();
    EXPECT_TRUE(stored.is_some());
    EXPECT_EQ(2u, stored.unwrap().num_shards);
}

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, InitializerInitializesLocalCache) {
    int port = next_port();

    // Start c-node server
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify cache not initialized
    EXPECT_FALSE(get_sharding_policy_cache().is_initialized());

    // Send TPC-C policy from initializer
    std::string addr = "127.0.0.1:" + std::to_string(port);
    EXPECT_TRUE(mako::send_tpcc_sharding_policy_to_cnode(addr, 10, 2));

    // Verify local cache is initialized
    EXPECT_TRUE(get_sharding_policy_cache().is_initialized());
}

// @unsafe - RPC I/O
TEST_F(ShardingStartupTest, InitializerFailsOnInvalidParams) {
    std::string addr = "127.0.0.1:12345";  // Non-existent server

    // Invalid parameters should fail gracefully
    EXPECT_FALSE(mako::send_tpcc_sharding_policy_to_cnode(addr, 0, 2));  // Zero warehouses
    EXPECT_FALSE(mako::send_tpcc_sharding_policy_to_cnode(addr, 10, 0)); // Zero shards
    EXPECT_FALSE(mako::send_tpcc_sharding_policy_to_cnode("", 10, 2));   // Empty addr
}

// =============================================================================
// Data Node Tests
// =============================================================================

// @unsafe - RPC I/O
TEST_F(ShardingStartupTest, DataNodeFetchesFromCNode) {
    int port = next_port();

    // Start c-node server with policy
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    auto policy = create_sample_policy();
    EXPECT_TRUE(cnode.as_ref().unwrap()->store->save_sharding_policy(policy));

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Clear local cache
    get_sharding_policy_cache().clear();
    EXPECT_FALSE(get_sharding_policy_cache().is_initialized());

    // Fetch policy via ConfigClient
    std::string addr = "127.0.0.1:" + std::to_string(port);
    ConfigClient client(addr);
    EXPECT_TRUE(client.connect());

    auto fetched = client.fetch_sharding_policy();
    EXPECT_TRUE(fetched.is_some());

    // Initialize local cache with fetched policy
    get_sharding_policy_cache().set_policy(fetched.unwrap());
    EXPECT_TRUE(get_sharding_policy_cache().is_initialized());

    client.disconnect();
}

// @unsafe - RPC I/O
TEST_F(ShardingStartupTest, DataNodeFallsBackWhenNoPolicyExists) {
    int port = next_port();

    // Start c-node server WITHOUT policy
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify no policy on c-node
    std::string addr = "127.0.0.1:" + std::to_string(port);
    ConfigClient client(addr);
    EXPECT_TRUE(client.connect());

    auto has_policy = client.has_sharding_policy();
    EXPECT_TRUE(has_policy.is_some());
    EXPECT_FALSE(has_policy.unwrap());

    // Data node should handle this gracefully (use fallback routing)
    auto fetched = client.fetch_sharding_policy();
    EXPECT_FALSE(fetched.is_some());

    client.disconnect();
}

// =============================================================================
// End-to-End Flow Tests
// =============================================================================

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, FullStartupFlow) {
    int port = next_port();

    // Step 1: C-node starts (first boot, no policy)
    auto cnode = create_cnode_server(port);
    ASSERT_TRUE(cnode.is_some());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(cnode.as_ref().unwrap()->store->has_sharding_policy());

    // Step 2: Initializer sends policy to c-node
    std::string addr = "127.0.0.1:" + std::to_string(port);
    EXPECT_TRUE(mako::send_tpcc_sharding_policy_to_cnode(addr, 10, 2));

    EXPECT_TRUE(cnode.as_ref().unwrap()->store->has_sharding_policy());

    // Step 3: Data node fetches policy from c-node
    get_sharding_policy_cache().clear();

    ConfigClient data_node_client(addr);
    EXPECT_TRUE(data_node_client.connect());

    auto has_policy = data_node_client.has_sharding_policy();
    EXPECT_TRUE(has_policy.is_some());
    EXPECT_TRUE(has_policy.unwrap());

    auto fetched = data_node_client.fetch_sharding_policy();
    EXPECT_TRUE(fetched.is_some());

    // Initialize local cache
    get_sharding_policy_cache().set_policy(fetched.unwrap());
    EXPECT_TRUE(get_sharding_policy_cache().is_initialized());

    // Verify routing works
    // TPC-C with 10 warehouses, 2 shards: w_id 1-5 -> shard 0, w_id 6-10 -> shard 1
    EXPECT_EQ(0, get_sharding_policy_cache().get_shard_for_key("WAREHOUSE", 1));
    EXPECT_EQ(0, get_sharding_policy_cache().get_shard_for_key("WAREHOUSE", 5));
    EXPECT_EQ(1, get_sharding_policy_cache().get_shard_for_key("WAREHOUSE", 6));
    EXPECT_EQ(1, get_sharding_policy_cache().get_shard_for_key("WAREHOUSE", 10));

    data_node_client.disconnect();
}

// @unsafe - RPC and RocksDB I/O
TEST_F(ShardingStartupTest, StartupAfterReboot) {
    int port = next_port();

    // First boot: initializer sets policy
    {
        auto cnode = create_cnode_server(port);
        ASSERT_TRUE(cnode.is_some());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::string addr = "127.0.0.1:" + std::to_string(port);
        EXPECT_TRUE(mako::send_tpcc_sharding_policy_to_cnode(addr, 10, 2));

        // C-node shuts down (destructor closes store)
    }

    // Simulate reboot: new port, same db path
    int reboot_port = next_port();
    {
        auto cnode = create_cnode_server(reboot_port);
        ASSERT_TRUE(cnode.is_some());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Policy should be available from RocksDB
        EXPECT_TRUE(cnode.as_ref().unwrap()->store->has_sharding_policy());

        // Data node can still fetch
        get_sharding_policy_cache().clear();

        std::string addr = "127.0.0.1:" + std::to_string(reboot_port);
        ConfigClient client(addr);
        EXPECT_TRUE(client.connect());

        auto fetched = client.fetch_sharding_policy();
        EXPECT_TRUE(fetched.is_some());
        EXPECT_EQ(2u, fetched.unwrap().num_shards);

        client.disconnect();
    }
}

}  // namespace test
}  // namespace janus

// @safe - Main entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
