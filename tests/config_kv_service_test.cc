// Unit tests for the ConfigKvService server-side read logic.
//
// The RPC handler ReadConfigKey delegates to DoReadConfigKey(key, &value),
// which is just a KvStore read — that logic is what we exercise here,
// over an InMemoryKvStore, with no socket / DeferredReply. The wire
// transport (proxy->ReadConfigKey over a real connection) and the
// make_config_read_fn client path are runtime-verified in CI against a
// live cluster.

#include <string>

#include <gtest/gtest.h>

#include "config_kv_service.h"
#include "cluster/in_memory_kv_store.h"

namespace janus {
namespace {

TEST(ConfigKvServiceTest, DoReadConfigKeyReturnsValueOnHit) {
    InMemoryKvStore kv;
    kv.put("shard_count", "3");
    kv.put("__version__", "7");

    ConfigKvServiceImpl svc(&kv);

    std::string out;
    ASSERT_TRUE(svc.DoReadConfigKey("shard_count", &out));
    EXPECT_EQ(out, "3");
    ASSERT_TRUE(svc.DoReadConfigKey("__version__", &out));
    EXPECT_EQ(out, "7");
}

TEST(ConfigKvServiceTest, DoReadConfigKeyReturnsFalseOnMiss) {
    InMemoryKvStore kv;
    ConfigKvServiceImpl svc(&kv);

    std::string out = "sentinel";
    EXPECT_FALSE(svc.DoReadConfigKey("absent", &out));
}

TEST(ConfigKvServiceTest, DoReadConfigKeyPreservesRawBytes) {
    // Serialized sharding-policy values contain embedded NULs.
    InMemoryKvStore kv;
    static const char kBytes[] = {'a', '\x00', 'b', '\x00', 'c'};
    const std::string payload(kBytes, sizeof(kBytes));
    kv.put("sharding/policy/WAREHOUSE", payload);

    ConfigKvServiceImpl svc(&kv);
    std::string out;
    ASSERT_TRUE(svc.DoReadConfigKey("sharding/policy/WAREHOUSE", &out));
    EXPECT_EQ(out, payload);
    EXPECT_EQ(out.size(), 5u);
}

TEST(ConfigKvServiceTest, DoReadConfigKeyNullBackingStoreIsSafe) {
    ConfigKvServiceImpl svc(nullptr);
    std::string out;
    EXPECT_FALSE(svc.DoReadConfigKey("k", &out));
}

}  // namespace
}  // namespace janus
