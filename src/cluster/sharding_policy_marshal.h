#pragma once

// rrr::Marshal serialization operators for the sharding-policy value
// types. Kept out of sharding_policy.h so that ClusterConfig and the
// standalone test_config_manager binary can depend on the policy types
// without pulling the rrr module into their link line.
//
// Includers of THIS header must also `#include "rrr/rrr.hpp"` before
// including this file, or link a target that provides the rrr module.

#include "sharding_policy.h"

namespace janus {

// @unsafe - Marshal I/O; bodies live in sharding_policy_marshal.cc
rrr::Marshal& operator<<(rrr::Marshal& m, const KeyExtractor& e);
rrr::Marshal& operator>>(rrr::Marshal& m, KeyExtractor& e);
rrr::Marshal& operator<<(rrr::Marshal& m, const RangeMapping& r);
rrr::Marshal& operator>>(rrr::Marshal& m, RangeMapping& r);
rrr::Marshal& operator<<(rrr::Marshal& m, const TableShardingPolicy& p);
rrr::Marshal& operator>>(rrr::Marshal& m, TableShardingPolicy& p);
rrr::Marshal& operator<<(rrr::Marshal& m, const ShardingPolicySet& s);
rrr::Marshal& operator>>(rrr::Marshal& m, ShardingPolicySet& s);

}  // namespace janus
