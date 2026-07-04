// ShardingPolicyCache is authored in the inline-Rust DSL in
// sharding_policy_cache.h (struct + methods generate as inline functions
// there, plus the spc_* C++ kernels and the get_sharding_policy_cache()
// singleton). This TU is intentionally just the include.
#include "sharding_policy_cache.h"
