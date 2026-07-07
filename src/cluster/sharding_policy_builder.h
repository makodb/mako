module;

/**
 * @file sharding_policy_builder.h
 * @brief Builder for ShardingPolicySet, authored in the inline-Rust DSL.
 *
 * Reshaped to be DSL-friendly (docs/storage-interface.md): the old fluent
 * TablePolicyBuilder (which returned `*this`, held a parent reference, and
 * owned a raw TablePolicyBuilder* with manual delete) is gone. Callers now
 * build a TableShardingPolicy directly via its DSL factory + add_range +
 * default_shard, then hand it to ShardingPolicyBuilder::add_policy. And
 * validation returns a `rusty::Result` (Err carries a message) rather than
 * throwing — so the whole builder, including build() and the
 * create_tpcc/create_uniform helpers, is DSL. The only C++ kernel left is
 * the literal TPC-C table-name list (spb_tpcc_tables) — a string-literal
 * array the DSL can't spell. Construction is pure DSL now that policies is
 * a rusty::Vec (Vec::<T>::new_() via turbofish).
 *
 * Usage (non-fluent):
 *   auto builder = ShardingPolicyBuilder::new_(num_shards);
 *   auto p = TableShardingPolicy::create("WAREHOUSE", KeyExtractor::by_field(0));
 *   p.add_range(0, 5, 0); p.default_shard = 0;
 *   builder.add_policy(p);
 *   rusty::Result<ShardingPolicySet, std::string> r = builder.build();
 */

#include <string>
#include <vector>
#include <cstdint>
#include <rusty/result.hpp>
#include <rusty/slice.hpp>
#include <rusty/vec.hpp>   // rusty::Vec<TableShardingPolicy> field

export module cluster:sharding_policy_builder;
import :sharding_policy;

export namespace janus {

// @safe - the fixed TPC-C table-name list (a C string-literal array the
// DSL can't spell).
inline std::vector<std::string> spb_tpcc_tables() {
    return {"WAREHOUSE", "DISTRICT", "CUSTOMER", "STOCK",
            "ORDER", "NEW_ORDER", "ORDER_LINE", "HISTORY", "ITEM",
            "warehouse", "district", "customer", "stock",
            "oorder", "new_order", "order_line", "history", "item"};
}

#if RUSTYCPP_RUST
pub struct ShardingPolicyBuilder {
    num_shards: i32,
    policies: rusty::Vec<TableShardingPolicy>,
}
impl ShardingPolicyBuilder {
    // Pure-DSL struct literal now that policies is a rusty::Vec (has new_()).
    fn new(num_shards: i32) -> ShardingPolicyBuilder {
        ShardingPolicyBuilder {
            num_shards: num_shards,
            policies: rusty::Vec::<TableShardingPolicy>::new_(),
        }
    }
    fn add_policy(&mut self, policy: TableShardingPolicy) {
        (*self).policies.push(policy);
    }
    fn get_num_shards(&self) -> i32 {
        (*self).num_shards
    }
    // Validate every table policy and assemble the ShardingPolicySet.
    // Returns Err(message) instead of throwing.
    fn build(&self) -> rusty::Result<ShardingPolicySet, std::string> {
        if (*self).policies.size() == 0 {
            return rusty::Err(std::string("ShardingPolicySet must have at least one table"));
        }
        let mut ti: usize = 0;
        while ti < (*self).policies.size() {
            if (*self).policies[ti].table_name.empty() {
                return rusty::Err(std::string("Table name cannot be empty"));
            }
            let mut ri: usize = 0;
            while ri < (*self).policies[ti].ranges.size() {
                let sid: i32 = (*self).policies[ti].ranges[ri].shard_id;
                if sid < 0 || sid >= (*self).num_shards {
                    return rusty::Err(std::string("Invalid shard_id for table"));
                }
                ri = ri + 1;
            }
            let ds: i32 = (*self).policies[ti].default_shard;
            if ds >= 0 && ds >= (*self).num_shards {
                return rusty::Err(std::string("Invalid default_shard for table"));
            }
            // Overlapping-range check (pairwise).
            let mut a: usize = 0;
            while a < (*self).policies[ti].ranges.size() {
                let mut b: usize = a + 1;
                while b < (*self).policies[ti].ranges.size() {
                    let r1s: i64 = (*self).policies[ti].ranges[a].start_key;
                    let r1e: i64 = (*self).policies[ti].ranges[a].end_key;
                    let r2s: i64 = (*self).policies[ti].ranges[b].start_key;
                    let r2e: i64 = (*self).policies[ti].ranges[b].end_key;
                    if r1s < r2e && r2s < r1e {
                        return rusty::Err(std::string("Overlapping ranges for table"));
                    }
                    b = b + 1;
                }
                a = a + 1;
            }
            ti = ti + 1;
        }
        let mut result: ShardingPolicySet = ShardingPolicySet::with_shards((*self).num_shards);
        result.version = 1;
        let mut i: usize = 0;
        while i < (*self).policies.size() {
            result.set_policy(&(*self).policies[i].table_name, &(*self).policies[i]);
            i = i + 1;
        }
        rusty::Ok(result)
    }
}

// TPC-C: all tables sharded by w_id (field 0), 1-indexed ranges.
pub fn create_tpcc_sharding_policy(num_warehouses: i32, num_shards: i32)
        -> rusty::Result<ShardingPolicySet, std::string> {
    if num_warehouses <= 0 || num_shards <= 0 {
        return rusty::Err(std::string("num_warehouses and num_shards must be positive"));
    }
    let wps: i32 = (num_warehouses + num_shards - 1) / num_shards;
    let mut builder: ShardingPolicyBuilder = ShardingPolicyBuilder::new(num_shards);
    let tables: std::vector<std::string> = unsafe { spb_tpcc_tables() };
    let mut t: usize = 0;
    while t < tables.size() {
        let ext: KeyExtractor = KeyExtractor::by_field(0);
        let tname: &std::string = &tables[t];
        let mut policy: TableShardingPolicy = TableShardingPolicy::create(tname, &ext);
        let mut s: i32 = 0;
        while s < num_shards {
            let start: i64 = ((s * wps) + 1) as i64;
            let end0: i64 = (((s + 1) * wps) + 1) as i64;
            let endw: i64 = (num_warehouses + 1) as i64;
            let end: i64 = if end0 < endw { end0 } else { endw };
            if start < end {
                policy.add_range(start, end, s);
            }
            s = s + 1;
        }
        policy.default_shard = 0;
        builder.add_policy(policy);
        t = t + 1;
    }
    builder.build()
}

// Uniform single-table sharding: keys 0..max_key split evenly.
pub fn create_uniform_sharding_policy(table_name: &std::string, key_field: i32,
                                      max_key: i64, num_shards: i32)
        -> rusty::Result<ShardingPolicySet, std::string> {
    if max_key <= 0 || num_shards <= 0 {
        return rusty::Err(std::string("max_key and num_shards must be positive"));
    }
    let kps: i64 = (max_key + (num_shards as i64) - 1) / (num_shards as i64);
    let mut builder: ShardingPolicyBuilder = ShardingPolicyBuilder::new(num_shards);
    let ext: KeyExtractor = KeyExtractor::by_field(key_field);
    let mut policy: TableShardingPolicy = TableShardingPolicy::create(table_name, &ext);
    let mut s: i32 = 0;
    while s < num_shards {
        let start: i64 = (s as i64) * kps;
        let end0: i64 = ((s + 1) as i64) * kps;
        let end: i64 = if end0 < max_key { end0 } else { max_key };
        if start < end {
            policy.add_range(start, end, s);
        }
        s = s + 1;
    }
    policy.default_shard = 0;
    builder.add_policy(policy);
    builder.build()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy_builder.1 version=1 rust_sha256=c54692f9ca4d6ab8096f37ad91352d9f53817f8533984710bb073aaaad653618*/
struct ShardingPolicyBuilder;

struct ShardingPolicyBuilder {
    int32_t num_shards;
    rusty::Vec<TableShardingPolicy> policies;

    static ShardingPolicyBuilder new_(int32_t num_shards);
    void add_policy(TableShardingPolicy policy);
    int32_t get_num_shards() const;
    rusty::Result<ShardingPolicySet, std::string> build() const;
};

rusty::Result<ShardingPolicySet, std::string> create_tpcc_sharding_policy(int32_t num_warehouses, int32_t num_shards) {
    if ((rusty::detail::deref_if_pointer_like(num_warehouses) <= 0) || (rusty::detail::deref_if_pointer_like(num_shards) <= 0)) {
        return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("num_warehouses and num_shards must be positive"));
    }
    const int32_t wps = (((rusty::detail::deref_if_pointer_like(num_warehouses) + rusty::detail::deref_if_pointer_like(num_shards)) - static_cast<int32_t>(1))) / rusty::detail::deref_if_pointer_like(num_shards);
    ShardingPolicyBuilder builder = ShardingPolicyBuilder::new_(std::move(num_shards));
    const std::vector<std::string> tables = spb_tpcc_tables();
    size_t t = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(t) < tables.size()) {
        const KeyExtractor ext = KeyExtractor::by_field(0);
        const std::string& tname = tables[t];
        TableShardingPolicy policy = TableShardingPolicy::create(tname, ext);
        int32_t s = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(s) < rusty::detail::deref_if_pointer_like(num_shards)) {
            const int64_t start = static_cast<int64_t>((((rusty::detail::deref_if_pointer_like(s) * rusty::detail::deref_if_pointer_like(wps))) + 1));
            const int64_t end0 = static_cast<int64_t>((((((rusty::detail::deref_if_pointer_like(s) + 1)) * rusty::detail::deref_if_pointer_like(wps))) + 1));
            const int64_t endw = static_cast<int64_t>((rusty::detail::deref_if_pointer_like(num_warehouses) + 1));
            const int64_t end = (rusty::detail::deref_if_pointer_like(end0) < rusty::detail::deref_if_pointer_like(endw) ? end0 : endw);
            if (rusty::detail::deref_if_pointer_like(start) < rusty::detail::deref_if_pointer_like(end)) {
                policy.add_range(std::move(start), std::move(end), std::move(s));
            }
            s = rusty::detail::deref_if_pointer_like(s) + static_cast<int32_t>(1);
        }
        policy.default_shard = 0;
        builder.add_policy(std::move(policy));
        t = rusty::detail::deref_if_pointer_like(t) + static_cast<size_t>(1);
    }
    return builder.build();
}

rusty::Result<ShardingPolicySet, std::string> create_uniform_sharding_policy(const std::string& table_name, int32_t key_field, int64_t max_key, int32_t num_shards) {
    if ((rusty::detail::deref_if_pointer_like(max_key) <= 0) || (rusty::detail::deref_if_pointer_like(num_shards) <= 0)) {
        return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("max_key and num_shards must be positive"));
    }
    const int64_t kps = (((rusty::detail::deref_if_pointer_like(max_key) + ((static_cast<int64_t>(num_shards)))) - static_cast<int64_t>(1))) / ((static_cast<int64_t>(num_shards)));
    ShardingPolicyBuilder builder = ShardingPolicyBuilder::new_(std::move(num_shards));
    const KeyExtractor ext = KeyExtractor::by_field(std::move(key_field));
    TableShardingPolicy policy = TableShardingPolicy::create(table_name, ext);
    int32_t s = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(s) < rusty::detail::deref_if_pointer_like(num_shards)) {
        const int64_t start = ((static_cast<int64_t>(s))) * rusty::detail::deref_if_pointer_like(kps);
        const int64_t end0 = ((static_cast<int64_t>((rusty::detail::deref_if_pointer_like(s) + 1)))) * rusty::detail::deref_if_pointer_like(kps);
        const int64_t end = (rusty::detail::deref_if_pointer_like(end0) < rusty::detail::deref_if_pointer_like(max_key) ? end0 : max_key);
        if (rusty::detail::deref_if_pointer_like(start) < rusty::detail::deref_if_pointer_like(end)) {
            policy.add_range(std::move(start), std::move(end), std::move(s));
        }
        s = rusty::detail::deref_if_pointer_like(s) + static_cast<int32_t>(1);
    }
    policy.default_shard = 0;
    builder.add_policy(std::move(policy));
    return builder.build();
}


inline ShardingPolicyBuilder ShardingPolicyBuilder::new_(int32_t num_shards) {
    return ShardingPolicyBuilder{.num_shards = std::move(num_shards), .policies = rusty::Vec<TableShardingPolicy>::new_()};
}

inline void ShardingPolicyBuilder::add_policy(TableShardingPolicy policy) {
    ((*this)).policies.push(std::move(policy));
}

inline int32_t ShardingPolicyBuilder::get_num_shards() const {
    return ((*this)).num_shards;
}

inline rusty::Result<ShardingPolicySet, std::string> ShardingPolicyBuilder::build() const {
    if (((*this)).policies.size() == 0) {
        return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("ShardingPolicySet must have at least one table"));
    }
    size_t ti = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(ti) < ((*this)).policies.size()) {
        if (((*this)).policies[ti].table_name.empty()) {
            return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("Table name cannot be empty"));
        }
        size_t ri = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(ri) < ((*this)).policies[ti].ranges.size()) {
            const int32_t sid = ((*this)).policies[ti].ranges[ri].shard_id;
            if ((rusty::detail::deref_if_pointer_like(sid) < 0) || (rusty::detail::deref_if_pointer_like(sid) >= rusty::detail::deref_if_pointer_like(((*this)).num_shards))) {
                return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("Invalid shard_id for table"));
            }
            ri = rusty::detail::deref_if_pointer_like(ri) + static_cast<size_t>(1);
        }
        const int32_t ds = ((*this)).policies[ti].default_shard;
        if ((rusty::detail::deref_if_pointer_like(ds) >= 0) && (rusty::detail::deref_if_pointer_like(ds) >= rusty::detail::deref_if_pointer_like(((*this)).num_shards))) {
            return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("Invalid default_shard for table"));
        }
        size_t a = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(a) < ((*this)).policies[ti].ranges.size()) {
            size_t b = rusty::detail::deref_if_pointer_like(a) + static_cast<size_t>(1);
            while (rusty::detail::deref_if_pointer_like(b) < ((*this)).policies[ti].ranges.size()) {
                const int64_t r1s = ((*this)).policies[ti].ranges[a].start_key;
                const int64_t r1e = ((*this)).policies[ti].ranges[a].end_key;
                const int64_t r2s = ((*this)).policies[ti].ranges[b].start_key;
                const int64_t r2e = ((*this)).policies[ti].ranges[b].end_key;
                if ((rusty::detail::deref_if_pointer_like(r1s) < rusty::detail::deref_if_pointer_like(r2e)) && (rusty::detail::deref_if_pointer_like(r2s) < rusty::detail::deref_if_pointer_like(r1e))) {
                    return rusty::Result<ShardingPolicySet, std::string>::Err(std::string("Overlapping ranges for table"));
                }
                b = rusty::detail::deref_if_pointer_like(b) + static_cast<size_t>(1);
            }
            a = rusty::detail::deref_if_pointer_like(a) + static_cast<size_t>(1);
        }
        ti = rusty::detail::deref_if_pointer_like(ti) + static_cast<size_t>(1);
    }
    ShardingPolicySet result = ShardingPolicySet::with_shards(((*this)).num_shards);
    result.version = 1;
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).policies.size()) {
        result.set_policy(((*this)).policies[i].table_name, ((*this)).policies[i]);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return rusty::Result<ShardingPolicySet, std::string>::Ok(std::move(result));
}
/*RUSTYCPP:GEN-END id=sharding_policy_builder.1*/

// @safe - factory body (ShardingPolicyBuilder complete here).

}  // namespace janus
