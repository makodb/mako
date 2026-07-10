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
import :config_manager;          // janus::ConfigManager (seed_warehouse_partitions)

namespace mako {

#if RUSTYCPP_RUST
// Unified path: once the process-global ClusterConfig is populated
// (ConfigWatcher has loaded topology from shard 0's __mako_config__) it is
// the single source of truth for routing — per-table policy, hash-mod
// default, and dead-shard replacement reroute all live in
// ClusterConfig::get_shard_for_key. Gated on a nonzero shard_count; an empty
// ClusterConfig falls through to the legacy ShardingPolicyCache + table-ID
// path below.
// The partition-table-governed owner of (table_id, key), or -1 when the
// ClusterConfig does not govern the table (empty config / hash mode / no
// partition present) -- the caller keeps its legacy behavior then. Two ways a
// table is governed: a routing ALIAS (TPC-C-style per-warehouse physical
// indexes: "customer_0", "customer_remote_5" -- key bytes carry only the
// shard-LOCAL warehouse id, so the alias registered at open time maps the
// index to its logical table plus a fixed encoded-global-warehouse routing
// key; routing ignores the op's key bytes at index granularity), or the
// REGISTERED name itself having a partition table (raw-key ranges).
pub fn governed_owner_shard(table_id: i32, key: &std::string) -> i32 {
    if janus::get_cluster_config().get_shard_count() > 0 {
        let route_opt: rusty::Option<std::string> =
            get_table_registry().get_route_table(table_id);
        if route_opt.is_some() {
            let route_table: &std::string = route_opt.as_ref().unwrap();
            if janus::get_cluster_config().routes_table(route_table) {
                let key_opt: rusty::Option<std::string> =
                    get_table_registry().get_route_key(table_id);
                if key_opt.is_some() {
                    return janus::get_cluster_config()
                        .get_shard_for_key(route_table, key_opt.as_ref().unwrap()) as i32;
                }
            }
        }
        let mut table_name: std::string = std::string();
        let name_opt: rusty::Option<std::string> =
            get_table_registry().get_table_name(table_id);
        if name_opt.is_some() {
            table_name = name_opt.as_ref().unwrap();
        }
        // Route through the ClusterConfig ONLY for tables it explicitly governs
        // (map mode + a partition table present). Everything else falls through
        // to the legacy path -- otherwise merely POPULATING the config (the
        // ConfigWatcher coming up) hijacks routing for tables that never
        // onboarded, e.g. TPC-C's warehouse-policy tables (observed live: 98%
        // remote-abort ratio on the one node whose watcher ran).
        if janus::get_cluster_config().routes_table(&table_name) {
            return janus::get_cluster_config().get_shard_for_key(&table_name, key) as i32;
        }
    }
    -1
}

pub fn compute_shard_for_key(table_id: i32, key: &std::string) -> i32 {
    let governed: i32 = mako::governed_owner_shard(table_id, key);
    if governed >= 0 {
        return governed;
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

// Partition-governed shard for a global warehouse under a logical table, or
// -1 when the table is ungoverned (caller falls back to the legacy static
// layout). This is the workload's local-vs-remote oracle (WarehouseInShard):
// once the partition table owns routing, flipping a warehouse's segment
// reroutes new transactions without touching the workload.
pub fn route_shard_for_warehouse(logical_table: &std::string, global_wid: i32) -> i32 {
    if janus::get_cluster_config().get_shard_count() > 0 {
        if janus::get_cluster_config().routes_table(logical_table) {
            let key: std::string = warehouse_route_key(global_wid);
            return janus::get_cluster_config().get_shard_for_key(logical_table, &key) as i32;
        }
    }
    -1
}

// Seed a logical table's partition table with the static TPC-C warehouse
// layout: one segment per shard, split points at warehouse boundaries in the
// warehouse_route_key encoding (segment 0 starts at "" = -inf so the whole
// keyspace is tiled). Idempotent: a table that already has a partition table
// keeps it (it may carry live migration cutovers). Mirrors the legacy layout
// wps = ceil(nw/ns), shard s owns warehouses [s*wps+1, min((s+1)*wps, nw)].
pub fn seed_warehouse_partitions(cm: *mut janus::ConfigManager, table: &std::string,
                                 num_warehouses_total: i32, num_shards: i32) -> bool {
    if num_warehouses_total <= 0 || num_shards <= 0 { return false; }
    if unsafe { (*cm).get_partition_count(table) } > 0 { return true; }
    let wps: i32 = (num_warehouses_total + num_shards - 1) / num_shards;
    let mut seg: u32 = 0;
    let mut s: i32 = 0;
    while s < num_shards {
        let first_wid: i32 = (s * wps) + 1;
        if first_wid <= num_warehouses_total {
            let start: std::string =
                if s == 0 { std::string("") } else { warehouse_route_key(first_wid) };
            unsafe { (*cm).put_partition_segment(table, seg, &start, s as u32); }
            seg = seg + 1;
        }
        s = s + 1;
    }
    unsafe { (*cm).set_partition_count(table, seg) }
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
/*RUSTYCPP:GEN-BEGIN id=shard_router.1 version=1 rust_sha256=8ff258b0bd1febee9ad15992c78cae0047046c50ef6c8f9d1a6b77c43536d87e*/
int32_t governed_owner_shard(int32_t table_id, const std::string& key);
int32_t compute_shard_for_key(int32_t table_id, const std::string& key);
int32_t compute_shard_for_key_value(int32_t table_id, const std::string& table_name, int64_t key_value);
int32_t route_shard_for_warehouse(const std::string& logical_table, int32_t global_wid);
bool has_policy_routing(const std::string& table_name);
int32_t get_policy_num_shards();

int32_t governed_owner_shard(int32_t table_id, const std::string& key) {
    if (janus::get_cluster_config().get_shard_count() > 0) {
        const rusty::Option<std::string> route_opt = get_table_registry().get_route_table(std::move(table_id));
        if (route_opt.is_some()) {
            const std::string& route_table = route_opt.as_ref().unwrap();
            if (janus::get_cluster_config().routes_table(route_table)) {
                const rusty::Option<std::string> key_opt = get_table_registry().get_route_key(std::move(table_id));
                if (key_opt.is_some()) {
                    return static_cast<int32_t>(janus::get_cluster_config().get_shard_for_key(route_table, key_opt.as_ref().unwrap()));
                }
            }
        }
        std::string table_name = std::string();
        const rusty::Option<std::string> name_opt = get_table_registry().get_table_name(std::move(table_id));
        if (name_opt.is_some()) {
            table_name = name_opt.as_ref().unwrap();
        }
        if (janus::get_cluster_config().routes_table(table_name)) {
            return static_cast<int32_t>(janus::get_cluster_config().get_shard_for_key(table_name, key));
        }
    }
    return -1;
}

int32_t compute_shard_for_key(int32_t table_id, const std::string& key) {
    int32_t governed = mako::governed_owner_shard(std::move(table_id), key);
    if (rusty::detail::deref_if_pointer_like(governed) >= 0) {
        return std::move(governed);
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

int32_t route_shard_for_warehouse(const std::string& logical_table, int32_t global_wid) {
    if (janus::get_cluster_config().get_shard_count() > 0) {
        if (janus::get_cluster_config().routes_table(logical_table)) {
            const std::string key = warehouse_route_key(std::move(global_wid));
            return static_cast<int32_t>(janus::get_cluster_config().get_shard_for_key(logical_table, key));
        }
    }
    return -1;
}

bool seed_warehouse_partitions(janus::ConfigManager* cm, const std::string& table, int32_t num_warehouses_total, int32_t num_shards) {
    if ((rusty::detail::deref_if_pointer_like(num_warehouses_total) <= 0) || (rusty::detail::deref_if_pointer_like(num_shards) <= 0)) {
        return false;
    }
    if (((*cm)).get_partition_count(table) > 0) {
        return true;
    }
    const int32_t wps = (((rusty::detail::deref_if_pointer_like(num_warehouses_total) + rusty::detail::deref_if_pointer_like(num_shards)) - static_cast<int32_t>(1))) / rusty::detail::deref_if_pointer_like(num_shards);
    uint32_t seg = static_cast<uint32_t>(0);
    int32_t s = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(s) < rusty::detail::deref_if_pointer_like(num_shards)) {
        const int32_t first_wid = ((rusty::detail::deref_if_pointer_like(s) * rusty::detail::deref_if_pointer_like(wps))) + static_cast<int32_t>(1);
        if (rusty::detail::deref_if_pointer_like(first_wid) <= rusty::detail::deref_if_pointer_like(num_warehouses_total)) {
            const std::string start = (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(0) ? std::string("") : warehouse_route_key(std::move(first_wid)));
            // @unsafe
            {
                ((*cm)).put_partition_segment(table, std::move(seg), start, static_cast<uint32_t>(s));
            }
            seg = rusty::detail::deref_if_pointer_like(seg) + static_cast<uint32_t>(1);
        }
        s = rusty::detail::deref_if_pointer_like(s) + static_cast<int32_t>(1);
    }
    // @unsafe
    {
        return ((*cm)).set_partition_count(table, std::move(seg));
    }
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
