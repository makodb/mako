/**
 * @file shard_router.cc
 * @brief Implementation of shard routing functions.
 *
 * This file is compiled as part of txlog library to have access to
 * deptran headers (ShardingPolicyCache, etc.).
 *
 * NOTE: We avoid including mako headers that pull in lib/common.h or rpc.h
 * to prevent symbol conflicts between mako and deptran.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the four
 * routing functions are the `#if RUSTYCPP_RUST` block below (regenerate
 * with scripts/regen_storage_dsl.sh). They are free `pub fn`s, so the
 * generated definitions land here in the .cc — compiled exactly once,
 * matching the declarations in shard_router.h — rather than in a header
 * (where non-inline free fns would be an ODR hazard). The bodies only do
 * control flow + method calls into ClusterConfig / ShardingPolicyCache /
 * the table registry, so no C++ kernels are needed; the header stays a
 * minimal set of declarations.
 */

module;
#include <string>
#include <cstdint>
#include "mako/lib/table_registry.h"   // plain header: mako::get_table_registry()
#include <rusty/slice.hpp>             // deref_if_pointer_like (generated bodies)
#include <rusty/option.hpp>            // get_table_name() -> rusty::Option<std::string>
module cluster;
import :shard_router;            // this partition's decls + SHARD_ROUTER_NUM_TABLES_PER_SHARD
import :cluster_config;          // janus::get_cluster_config()
import :sharding_policy_cache;   // janus::get_sharding_policy_cache()

namespace mako {

#if RUSTYCPP_RUST
// Unified path: once the process-global ClusterConfig is populated
// (ConfigWatcher has loaded topology from shard 0's __mako_config__) it is
// the single source of truth for routing — per-table policy, hash-mod
// default, and dead-shard replacement reroute all live in
// ClusterConfig::get_shard_for_key. Gated on a nonzero shard_count; an empty
// ClusterConfig falls through to the legacy ShardingPolicyCache + table-ID
// path below.
pub fn compute_shard_for_key(table_id: i32, key: &std::string) -> i32 {
    if janus::get_cluster_config().get_shard_count() > 0 {
        let mut table_name: std::string = std::string();
        let name_opt: rusty::Option<std::string> =
            get_table_registry().get_table_name(table_id);
        if name_opt.is_some() {
            table_name = name_opt.as_ref().unwrap();
        }
        // Route through the ClusterConfig ONLY for tables it explicitly governs
        // (map mode + a partition table present). Everything else falls through
        // to the legacy path below -- otherwise merely POPULATING the config
        // (the ConfigWatcher coming up) hijacks routing for tables that never
        // onboarded, e.g. TPC-C's warehouse-policy tables (observed live: 98%
        // remote-abort ratio on the one node whose watcher ran).
        if janus::get_cluster_config().routes_table(&table_name) {
            return janus::get_cluster_config().get_shard_for_key(&table_name, key) as i32;
        }
    }

    // Legacy path: ShardingPolicyCache + table-ID fallback.
    if janus::get_sharding_policy_cache().is_initialized() {
        let name_opt: rusty::Option<std::string> =
            get_table_registry().get_table_name(table_id);
        if name_opt.is_some() {
            let table_name: &std::string = name_opt.as_ref().unwrap();
            if janus::get_sharding_policy_cache().has_policy_for_table(table_name) {
                if !key.empty() {
                    // First 8 key bytes as a big-endian int64 sharding field.
                    let mut key_value: i64 = 0;
                    let n: usize = if key.size() < 8 { key.size() } else { 8 };
                    let mut i: usize = 0;
                    while i < n {
                        key_value = (key_value << 8) | ((key[i] as u8) as i64);
                        i = i + 1;
                    }
                    let shard: i32 = janus::get_sharding_policy_cache()
                        .get_shard_for_key(table_name, key_value);
                    if shard >= 0 {
                        return shard;
                    }
                }
            }
        }
    }

    // Fall back to table-ID-based routing.
    (table_id - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD
}

// key_value already extracted (e.g. warehouse_id): policy lookup, then the
// table-ID fallback.
pub fn compute_shard_for_key_value(table_id: i32, table_name: &std::string,
                                   key_value: i64) -> i32 {
    if janus::get_sharding_policy_cache().is_initialized()
        && janus::get_sharding_policy_cache().has_policy_for_table(table_name) {
        let shard: i32 = janus::get_sharding_policy_cache()
            .get_shard_for_key(table_name, key_value);
        if shard >= 0 {
            return shard;
        }
    }
    (table_id - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD
}

pub fn has_policy_routing(table_name: &std::string) -> bool {
    janus::get_sharding_policy_cache().is_initialized()
        && janus::get_sharding_policy_cache().has_policy_for_table(table_name)
}

pub fn get_policy_num_shards() -> i32 {
    if janus::get_sharding_policy_cache().is_initialized() {
        return janus::get_sharding_policy_cache().get_num_shards();
    }
    0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard_router.1 version=1 rust_sha256=6ffc13f5cabc93ad4321f03dcd483d5716cbdbd878956abbc46ab0fb98dc33a8*/
int32_t compute_shard_for_key(int32_t table_id, const std::string& key);
int32_t compute_shard_for_key_value(int32_t table_id, const std::string& table_name, int64_t key_value);
bool has_policy_routing(const std::string& table_name);
int32_t get_policy_num_shards();

int32_t compute_shard_for_key(int32_t table_id, const std::string& key) {
    if (janus::get_cluster_config().get_shard_count() > 0) {
        std::string table_name = std::string();
        const rusty::Option<std::string> name_opt = get_table_registry().get_table_name(std::move(table_id));
        if (name_opt.is_some()) {
            table_name = name_opt.as_ref().unwrap();
        }
        if (janus::get_cluster_config().routes_table(table_name)) {
            return static_cast<int32_t>(janus::get_cluster_config().get_shard_for_key(table_name, key));
        }
    }
    if (janus::get_sharding_policy_cache().is_initialized()) {
        const rusty::Option<std::string> name_opt = get_table_registry().get_table_name(std::move(table_id));
        if (name_opt.is_some()) {
            const std::string& table_name = name_opt.as_ref().unwrap();
            if (janus::get_sharding_policy_cache().has_policy_for_table(table_name)) {
                if (!key.empty()) {
                    int64_t key_value = static_cast<int64_t>(0);
                    const size_t n = (key.size() < 8 ? key.size() : static_cast<size_t>(8));
                    size_t i = static_cast<size_t>(0);
                    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
                        key_value = ((rusty::detail::deref_if_pointer_like(key_value) << 8)) | ((static_cast<int64_t>((static_cast<uint8_t>(key[i])))));
                        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
                    }
                    int32_t shard = janus::get_sharding_policy_cache().get_shard_for_key(table_name, std::move(key_value));
                    if (rusty::detail::deref_if_pointer_like(shard) >= 0) {
                        return std::move(shard);
                    }
                }
            }
        }
    }
    return ((rusty::detail::deref_if_pointer_like(table_id) - static_cast<int32_t>(1))) / rusty::detail::deref_if_pointer_like(SHARD_ROUTER_NUM_TABLES_PER_SHARD);
}

int32_t compute_shard_for_key_value(int32_t table_id, const std::string& table_name, int64_t key_value) {
    if (janus::get_sharding_policy_cache().is_initialized() && janus::get_sharding_policy_cache().has_policy_for_table(table_name)) {
        int32_t shard = janus::get_sharding_policy_cache().get_shard_for_key(table_name, std::move(key_value));
        if (rusty::detail::deref_if_pointer_like(shard) >= 0) {
            return std::move(shard);
        }
    }
    return ((rusty::detail::deref_if_pointer_like(table_id) - static_cast<int32_t>(1))) / rusty::detail::deref_if_pointer_like(SHARD_ROUTER_NUM_TABLES_PER_SHARD);
}

bool has_policy_routing(const std::string& table_name) {
    return janus::get_sharding_policy_cache().is_initialized() && janus::get_sharding_policy_cache().has_policy_for_table(table_name);
}

int32_t get_policy_num_shards() {
    if (janus::get_sharding_policy_cache().is_initialized()) {
        return janus::get_sharding_policy_cache().get_num_shards();
    }
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=shard_router.1*/

}  // namespace mako
