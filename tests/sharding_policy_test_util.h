#pragma once

/**
 * @file sharding_policy_test_util.h
 * @brief Test-only helpers for building ShardingPolicy values the
 *        DSL-friendly (non-fluent) way.
 *
 * The old fluent ShardingPolicyBuilder (.table(...).shardBy...().addRange(...))
 * is gone; the builder is now DSL and validation returns a rusty::Result.
 * These inline helpers keep the tests terse: build a TableShardingPolicy from
 * a KeyExtractor + a list of (start, end, shard) ranges, and assemble a
 * validated ShardingPolicySet from one or more of them.
 */

#include <array>
#include <initializer_list>
#include <string>

import cluster;   // config/sharding metadata module (was #include "cluster/...")

namespace janus {

// Build a TableShardingPolicy: create it from a KeyExtractor, push each
// (start, end, shard) range, set the default shard (-1 = "no match is an
// error"). Replaces the old fluent .table(...).shardBy...().addRange(...).
inline TableShardingPolicy make_table_policy(
        const std::string& name, KeyExtractor ext,
        std::initializer_list<std::array<int64_t, 3>> ranges,
        int32_t default_shard = -1) {
    TableShardingPolicy p = TableShardingPolicy::create(name, ext);
    for (const auto& r : ranges) {
        p.add_range(r[0], r[1], static_cast<int32_t>(r[2]));
    }
    p.default_shard = default_shard;
    return p;
}

// Assemble a validated ShardingPolicySet from a set of table policies.
// Unwraps the builder's Result (tests here only pass valid inputs).
inline ShardingPolicySet make_policy_set(
        int32_t num_shards,
        std::initializer_list<TableShardingPolicy> policies) {
    ShardingPolicyBuilder builder = ShardingPolicyBuilder::new_(num_shards);
    for (const auto& p : policies) {
        builder.add_policy(p);
    }
    return builder.build().unwrap();
}

}  // namespace janus
