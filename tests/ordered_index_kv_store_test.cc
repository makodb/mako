// Verifies the OrderedIndexKvStore adapter — the seam that binds the
// cluster KvStore port onto Mako's unified FullOrderedIndex. This is
// the "when we run them together" test: unlike test_config_manager
// (pure cluster, no storage headers), this TU pulls the storage
// interface + an in-memory FullOrderedIndex fake to confirm the shim
// round-trips point ops correctly.

#include <gtest/gtest.h>

#include "cluster/kv_store.h"
#include "mako/in_memory_ordered_index.h"
#include "mako/ordered_index_kv_store.h"

namespace janus {

TEST(OrderedIndexKvStoreTest, PutGetRoundTrip) {
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    kv.put("shard/0/replicas", "s1,s2,s3");
    std::string out;
    ASSERT_TRUE(kv.get("shard/0/replicas", &out));
    EXPECT_EQ(out, "s1,s2,s3");
}

TEST(OrderedIndexKvStoreTest, GetMissReturnsFalse) {
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    std::string out = "sentinel";
    EXPECT_FALSE(kv.get("absent", &out));
}

TEST(OrderedIndexKvStoreTest, OverwriteAndRemove) {
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    kv.put("__version__", "1");
    kv.put("__version__", "2");  // blind overwrite
    std::string out;
    ASSERT_TRUE(kv.get("__version__", &out));
    EXPECT_EQ(out, "2");

    kv.remove("__version__");
    EXPECT_FALSE(kv.get("__version__", &out));
}

TEST(OrderedIndexKvStoreTest, RawByteValuesWithEmbeddedNul) {
    // The non-txn OrderedIndex surface is raw bytes; the adapter must
    // preserve embedded NULs (serialized sharding policies contain them).
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    static const char kBytes[] = {'a', '\x00', 'b', '\x00', 'c'};
    const std::string payload(kBytes, sizeof(kBytes));
    kv.put("sharding/policy/WAREHOUSE", payload);

    std::string out;
    ASSERT_TRUE(kv.get("sharding/policy/WAREHOUSE", &out));
    EXPECT_EQ(out, payload);
    EXPECT_EQ(out.size(), 5u);
}

TEST(OrderedIndexKvStoreTest, NullIndexIsSafe) {
    // Defensive: a null backing index must not crash.
    OrderedIndexKvStore kv(nullptr);
    std::string out;
    EXPECT_FALSE(kv.get("k", &out));
    kv.put("k", "v");     // no-op, no crash
    kv.remove("k");       // no-op, no crash
}

}  // namespace janus

