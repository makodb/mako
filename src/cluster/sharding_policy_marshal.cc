// Out-of-line Marshal serialization for the sharding-policy value
// types. These live in a separate translation unit so that including
// sharding_policy.h does NOT require rrr/rrr.hpp to be complete —
// which lets ClusterConfig and the standalone test_config_manager
// binary depend on the policy types without pulling the rrr module
// into their link line.

#include "rrr/rrr.hpp"
#include "sharding_policy_marshal.h"

namespace janus {

// @unsafe - Marshal I/O
rrr::Marshal& operator<<(rrr::Marshal& m, const KeyExtractor& e) {
    m << static_cast<int32_t>(e.type);
    m << e.field_index;
    m << e.prefix_length;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator>>(rrr::Marshal& m, KeyExtractor& e) {
    int32_t type_val;
    m >> type_val;
    e.type = static_cast<KeyExtractorType>(type_val);
    m >> e.field_index;
    m >> e.prefix_length;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator<<(rrr::Marshal& m, const RangeMapping& r) {
    m << r.start_key;
    m << r.end_key;
    m << r.shard_id;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator>>(rrr::Marshal& m, RangeMapping& r) {
    m >> r.start_key;
    m >> r.end_key;
    m >> r.shard_id;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator<<(rrr::Marshal& m, const TableShardingPolicy& p) {
    m << p.table_name;
    m << p.key_extractor;
    m << static_cast<int32_t>(p.ranges.size());
    for (const auto& range : p.ranges) {
        m << range;
    }
    m << p.default_shard;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator>>(rrr::Marshal& m, TableShardingPolicy& p) {
    m >> p.table_name;
    m >> p.key_extractor;
    int32_t num_ranges;
    m >> num_ranges;
    p.ranges.clear();
    p.ranges.reserve(num_ranges);
    for (int32_t i = 0; i < num_ranges; ++i) {
        RangeMapping range;
        m >> range;
        p.ranges.push_back(range);
    }
    m >> p.default_shard;
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator<<(rrr::Marshal& m, const ShardingPolicySet& s) {
    m << s.version;
    m << s.num_shards;
    m << static_cast<int32_t>(s.policies.size());
    for (const auto& [name, policy] : s.policies) {
        m << policy;
    }
    return m;
}

// @unsafe - Marshal I/O
rrr::Marshal& operator>>(rrr::Marshal& m, ShardingPolicySet& s) {
    m >> s.version;
    m >> s.num_shards;
    int32_t num_policies;
    m >> num_policies;
    s.policies.clear();
    for (int32_t i = 0; i < num_policies; ++i) {
        TableShardingPolicy policy;
        m >> policy;
        s.policies[policy.table_name] = policy;
    }
    return m;
}

}  // namespace janus
