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
    auto out = kv.get("shard/0/replicas");
    ASSERT_TRUE(out.is_some());
    EXPECT_EQ(out.unwrap(), "s1,s2,s3");
}

TEST(OrderedIndexKvStoreTest, GetMissReturnsFalse) {
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    EXPECT_TRUE(kv.get("absent").is_none());
}

TEST(OrderedIndexKvStoreTest, OverwriteAndRemove) {
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    kv.put("__version__", "1");
    kv.put("__version__", "2");  // blind overwrite
    auto out = kv.get("__version__");
    ASSERT_TRUE(out.is_some());
    EXPECT_EQ(out.unwrap(), "2");

    kv.remove("__version__");
    EXPECT_TRUE(kv.get("__version__").is_none());
}

TEST(OrderedIndexKvStoreTest, RawByteValuesWithEmbeddedNul) {
    // The non-txn OrderedIndex surface is raw bytes; the adapter must
    // preserve embedded NULs (serialized sharding policies contain them).
    InMemoryOrderedIndex index;
    OrderedIndexKvStore kv(&index);

    static const char kBytes[] = {'a', '\x00', 'b', '\x00', 'c'};
    const std::string payload(kBytes, sizeof(kBytes));
    kv.put("sharding/policy/WAREHOUSE", payload);

    auto found = kv.get("sharding/policy/WAREHOUSE");
    ASSERT_TRUE(found.is_some());
    std::string out = found.unwrap();
    EXPECT_EQ(out, payload);
    EXPECT_EQ(out.size(), 5u);
}

TEST(OrderedIndexKvStoreTest, NullIndexIsSafe) {
    // Defensive: a null backing index must not crash.
    OrderedIndexKvStore kv(nullptr);
    EXPECT_TRUE(kv.get("k").is_none());
    kv.put("k", "v");     // no-op, no crash
    kv.remove("k");       // no-op, no crash
}

}  // namespace janus

