/**
 * @file sharding_policy.h
 * @brief Data structures for range-based sharding policy
 *
 * This file defines the schema for user-defined sharding policies that
 * determine how data is distributed across shards based on key ranges.
 *
 * Example usage (TPC-C warehouse-based sharding):
 *   - All tables sharded by w_id (field 0 in composite keys)
 *   - Warehouses 0-4 go to shard 0, warehouses 5-9 go to shard 1
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

// deref_if_pointer_like + rusty::clone, used by the DSL-generated method
// bodies below (clone wraps the enum literals in the factory struct
// literals). rusty, NOT rrr — the standalone cluster test build already
// has this include path (sharding_policy_cache.h pulls rusty too), so it
// does not break the no-rrr guarantee.
#include <rusty/slice.hpp>
#include <rusty/move.hpp>

// NOTE: sharding_policy.h has ZERO dependency on rrr on purpose so it
// can be included from cluster/cluster_config.h without pulling the
// rrr module in — that is what keeps test_config_manager standalone.
// The rrr::Marshal serialization operators live in the separate
// header sharding_policy_marshal.h with bodies in
// sharding_policy_marshal.cc; anyone who needs to serialize should
// include that header directly.

namespace janus {

/**
 * Defines how to extract the sharding key from a row key.
 */
enum class KeyExtractorType : int32_t {
    FIELD_INDEX = 0,   // Extract nth field from composite key (e.g., w_id is field 0)
    PREFIX_BYTES = 1,  // Extract first N bytes and interpret as int64
    HASH_MOD = 2       // Hash entire key, mod by num_shards (fallback)
};

// @safe - Pure function converting enum to string
inline const char* key_extractor_type_to_string(KeyExtractorType type) {
    switch (type) {
        case KeyExtractorType::FIELD_INDEX: return "FIELD_INDEX";
        case KeyExtractorType::PREFIX_BYTES: return "PREFIX_BYTES";
        case KeyExtractorType::HASH_MOD: return "HASH_MOD";
        default: return "UNKNOWN";
    }
}

/**
 * Defines how to extract the sharding key from a composite row key.
 *
 * DSL value type (docs/storage-interface.md). The field is `kind`, not
 * `type` — `type` is a Rust keyword the DSL can't spell. Copyable
 * aggregate (plain struct + inherent impl); the marshal reader
 * default-constructs + fills every field. Use KeyExtractor::defaults()
 * for the old default-constructed values (FIELD_INDEX, 0, 4), or the
 * byField/byPrefix/byHash factories.
 */
#if RUSTYCPP_RUST
pub struct KeyExtractor {
    kind: KeyExtractorType,   // was `type`
    field_index: i32,         // For FIELD_INDEX: which field (0-based)
    prefix_length: i32,       // For PREFIX_BYTES: how many bytes to read
}
impl KeyExtractor {
    // The old default-constructed values.
    fn defaults() -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::FIELD_INDEX, field_index: 0, prefix_length: 4 }
    }
    // Replaces the old (kind, field, prefix) constructor.
    fn make(kind: KeyExtractorType, field: i32, prefix: i32) -> KeyExtractor {
        KeyExtractor { kind: kind, field_index: field, prefix_length: prefix }
    }
    fn byField(index: i32) -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::FIELD_INDEX, field_index: index, prefix_length: 0 }
    }
    fn byPrefix(length: i32) -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::PREFIX_BYTES, field_index: 0, prefix_length: length }
    }
    fn byHash() -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::HASH_MOD, field_index: 0, prefix_length: 0 }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.1 version=1 rust_sha256=63e25ae66989b21861772976f0a9fc65653197f30139b6e00ce4a7e1b4ba63de*/
struct KeyExtractor;

struct KeyExtractor {
    KeyExtractorType kind;
    int32_t field_index;
    int32_t prefix_length;

    static KeyExtractor defaults();
    static KeyExtractor make(KeyExtractorType kind, int32_t field, int32_t prefix);
    static KeyExtractor byField(int32_t index);
    static KeyExtractor byPrefix(int32_t length);
    static KeyExtractor byHash();
};


inline KeyExtractor KeyExtractor::defaults() {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::FIELD_INDEX)), .field_index = static_cast<int32_t>(0), .prefix_length = static_cast<int32_t>(4)};
}

inline KeyExtractor KeyExtractor::make(KeyExtractorType kind, int32_t field, int32_t prefix) {
    return KeyExtractor{.kind = std::move(kind), .field_index = std::move(field), .prefix_length = std::move(prefix)};
}

inline KeyExtractor KeyExtractor::byField(int32_t index) {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::FIELD_INDEX)), .field_index = std::move(index), .prefix_length = static_cast<int32_t>(0)};
}

inline KeyExtractor KeyExtractor::byPrefix(int32_t length) {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::PREFIX_BYTES)), .field_index = static_cast<int32_t>(0), .prefix_length = std::move(length)};
}

inline KeyExtractor KeyExtractor::byHash() {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::HASH_MOD)), .field_index = static_cast<int32_t>(0), .prefix_length = static_cast<int32_t>(0)};
}
/*RUSTYCPP:GEN-END id=sharding_policy.1*/

/**
 * Maps a key range [start_key, end_key) to a specific shard.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the
 * #if RUSTYCPP_RUST block is the source of truth, the GEN block is the
 * generated C++. Regenerate with scripts/regen_storage_dsl.sh. A plain
 * struct + inherent impl lowers to a copyable aggregate (no synthesized
 * ctor/move), so it still lives in std::vector by value and the marshal
 * reader (default-construct + field fill) is unchanged. NOTE: the DSL
 * cannot express default member initializers, so a bare `RangeMapping r;`
 * leaves fields indeterminate (previously 0); the only default-constructor
 * is the marshal reader, which overwrites every field immediately. Use
 * RangeMapping::make(...) for an initialized value.
 */
#if RUSTYCPP_RUST
pub struct RangeMapping {
    start_key: i64,   // Inclusive start of range
    end_key: i64,     // Exclusive end of range
    shard_id: i32,    // Target shard for this range
}
impl RangeMapping {
    // Factory preserving the old (start, end, shard) constructor.
    fn make(start: i64, end: i64, shard: i32) -> RangeMapping {
        RangeMapping { start_key: start, end_key: end, shard_id: shard }
    }
    // True if key is within [start_key, end_key).
    fn contains(&self, key: i64) -> bool {
        (*self).start_key <= key && key < (*self).end_key
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.2 version=1 rust_sha256=4b3baf906968525e8659e0c7149163b206e128fe66f8e554bd86de4475fea60e*/
struct RangeMapping;

struct RangeMapping {
    int64_t start_key;
    int64_t end_key;
    int32_t shard_id;

    static RangeMapping make(int64_t start, int64_t end, int32_t shard);
    bool contains(int64_t key) const;
};


inline RangeMapping RangeMapping::make(int64_t start, int64_t end, int32_t shard) {
    return RangeMapping{.start_key = std::move(start), .end_key = std::move(end), .shard_id = std::move(shard)};
}

inline bool RangeMapping::contains(int64_t key) const {
    return (rusty::detail::deref_if_pointer_like(((*this)).start_key) <= rusty::detail::deref_if_pointer_like(key)) && (rusty::detail::deref_if_pointer_like(key) < rusty::detail::deref_if_pointer_like(((*this)).end_key));
}
/*RUSTYCPP:GEN-END id=sharding_policy.2*/

/**
 * Sharding policy for a single table.
 */
// C++ kernels for the TableShardingPolicy DSL bodies below. add_range does
// an iterator insert (kept in C++ — the DSL should not hand-roll iterator
// surgery); get_shard is expressed directly in the DSL as a plain binary
// search. tsp_create is the factory replacing the old (name, extractor)
// constructor; it returns a complete TableShardingPolicy so its body lives
// below the generated struct.
struct TableShardingPolicy;  // for the factory kernel's forward declaration

// @unsafe - vector iterator insert, keeping ranges sorted by start_key
inline void tsp_add_range_sorted(std::vector<RangeMapping>* ranges,
                                 int64_t start, int64_t end, int32_t shard) {
    RangeMapping mapping = RangeMapping::make(start, end, shard);
    auto it = ranges->begin();
    while (it != ranges->end() && it->start_key < start) {
        ++it;
    }
    ranges->insert(it, mapping);
}

// @safe - factory: preserves the old ctor default (default_shard = -1),
// which the DSL cannot express as a field initializer. Defined below the
// struct; forward-declared here so the DSL body can call it.
inline TableShardingPolicy tsp_create(const std::string& name,
                                      const KeyExtractor& extractor);

#if RUSTYCPP_RUST
pub struct TableShardingPolicy {
    table_name: std::string,
    key_extractor: KeyExtractor,
    ranges: std::vector<RangeMapping>,   // Sorted by start_key for binary search
    default_shard: i32,                  // -1 means error if no range matches
}
impl TableShardingPolicy {
    // Factory replacing the old (name, extractor) constructor.
    fn create(name: &std::string, extractor: &KeyExtractor) -> TableShardingPolicy {
        unsafe { tsp_create(name, extractor) }
    }
    // Binary search for the range containing key_value (ranges are sorted
    // by start_key); returns default_shard when no range matches.
    fn get_shard(&self, key_value: i64) -> i32 {
        let mut left: i32 = 0;
        let mut right: i32 = ((*self).ranges.size() as i32) - 1;
        while left <= right {
            let mid: i32 = left + (right - left) / 2;
            if key_value < (*self).ranges[mid].start_key {
                right = mid - 1;
            } else if key_value >= (*self).ranges[mid].end_key {
                left = mid + 1;
            } else {
                return (*self).ranges[mid].shard_id;
            }
        }
        (*self).default_shard
    }
    // Insert a range, keeping ranges sorted by start_key.
    fn add_range(&mut self, start: i64, end: i64, shard: i32) {
        unsafe { tsp_add_range_sorted(&mut self.ranges, start, end, shard) }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.3 version=1 rust_sha256=f813f18abb5b558610eebb7a125eca26d27d52ecc18b2efc295156b19c12b6e2*/
struct TableShardingPolicy;

struct TableShardingPolicy {
    std::string table_name;
    KeyExtractor key_extractor;
    std::vector<RangeMapping> ranges;
    int32_t default_shard;

    static TableShardingPolicy create(const std::string& name, const KeyExtractor& extractor);
    int32_t get_shard(int64_t key_value) const;
    void add_range(int64_t start, int64_t end, int32_t shard);
};


inline TableShardingPolicy TableShardingPolicy::create(const std::string& name, const KeyExtractor& extractor) {
    // @unsafe
    {
        return tsp_create(name, extractor);
    }
}

inline int32_t TableShardingPolicy::get_shard(int64_t key_value) const {
    int32_t left = static_cast<int32_t>(0);
    int32_t right = ((static_cast<int32_t>(((*this)).ranges.size()))) - static_cast<int32_t>(1);
    while (rusty::detail::deref_if_pointer_like(left) <= rusty::detail::deref_if_pointer_like(right)) {
        const int32_t mid = rusty::detail::deref_if_pointer_like(left) + (((rusty::detail::deref_if_pointer_like(right) - rusty::detail::deref_if_pointer_like(left))) / static_cast<int32_t>(2));
        if (rusty::detail::deref_if_pointer_like(key_value) < rusty::detail::deref_if_pointer_like(((*this)).ranges[mid].start_key)) {
            right = rusty::detail::deref_if_pointer_like(mid) - static_cast<int32_t>(1);
        } else if (rusty::detail::deref_if_pointer_like(key_value) >= rusty::detail::deref_if_pointer_like(((*this)).ranges[mid].end_key)) {
            left = rusty::detail::deref_if_pointer_like(mid) + static_cast<int32_t>(1);
        } else {
            return ((*this)).ranges[mid].shard_id;
        }
    }
    return ((*this)).default_shard;
}

inline void TableShardingPolicy::add_range(int64_t start, int64_t end, int32_t shard) {
    // @unsafe
    {
        tsp_add_range_sorted(&this->ranges, std::move(start), std::move(end), std::move(shard));
    }
}
/*RUSTYCPP:GEN-END id=sharding_policy.3*/

// @safe - factory body (TableShardingPolicy is a complete type here)
inline TableShardingPolicy tsp_create(const std::string& name,
                                      const KeyExtractor& extractor) {
    TableShardingPolicy p{};
    p.table_name = name;
    p.key_extractor = extractor;
    p.default_shard = -1;
    return p;
}

/**
 * Complete sharding policy set containing all table policies.
 */
// C++ kernels for the ShardingPolicySet map lookups below (iterator /
// pointer surgery the DSL should not hand-roll). with_shards is the factory
// replacing the (num_shards) constructor.
struct ShardingPolicySet;  // for the factory kernel's forward declaration

// @unsafe - map find -> pointer-or-null
inline const TableShardingPolicy* sps_get_policy(
        const std::map<std::string, TableShardingPolicy>* policies,
        const std::string& table_name) {
    auto it = policies->find(table_name);
    return it != policies->end() ? &it->second : nullptr;
}
// @safe - blind map insert / overwrite
inline void sps_set_policy(std::map<std::string, TableShardingPolicy>* policies,
                           const std::string& table_name,
                           const TableShardingPolicy& policy) {
    (*policies)[table_name] = policy;
}
// @safe - delegate to the matched table policy's binary search
inline int32_t sps_get_shard_for_key(
        const std::map<std::string, TableShardingPolicy>* policies,
        const std::string& table_name, int64_t key_value) {
    const TableShardingPolicy* p = sps_get_policy(policies, table_name);
    return p == nullptr ? -1 : p->get_shard(key_value);
}
// @safe - membership test
inline bool sps_has_policy(
        const std::map<std::string, TableShardingPolicy>* policies,
        const std::string& table_name) {
    return policies->find(table_name) != policies->end();
}
// @safe - factory: preserves the old ctor (num_shards = shards). Defined
// below the struct; forward-declared here for the DSL body.
inline ShardingPolicySet sps_with_shards(int32_t shards);

#if RUSTYCPP_RUST
pub struct ShardingPolicySet {
    version: u64,                                          // cache-invalidation version
    num_shards: i32,                                       // total shards in the cluster
    policies: std::map<std::string, TableShardingPolicy>,  // table_name -> policy
}
impl ShardingPolicySet {
    // Factory replacing the old (shards) constructor. Sets num_shards
    // (the DSL cannot express the `= 1` field initializer).
    fn with_shards(shards: i32) -> ShardingPolicySet {
        unsafe { sps_with_shards(shards) }
    }
    // Policy for a table, or null if none is registered.
    fn get_policy(&self, table_name: &std::string) -> *const TableShardingPolicy {
        unsafe { sps_get_policy(&self.policies, table_name) }
    }
    // Add or overwrite a table's policy.
    fn set_policy(&mut self, table_name: &std::string, policy: &TableShardingPolicy) {
        unsafe { sps_set_policy(&mut self.policies, table_name, policy) }
    }
    // Route: shard for (table, key), or -1 if the table has no policy.
    fn get_shard_for_key(&self, table_name: &std::string, key_value: i64) -> i32 {
        unsafe { sps_get_shard_for_key(&self.policies, table_name, key_value) }
    }
    // True if a policy is registered for the table.
    fn has_policy(&self, table_name: &std::string) -> bool {
        unsafe { sps_has_policy(&self.policies, table_name) }
    }
    // Number of tables with a policy.
    fn table_count(&self) -> usize {
        (*self).policies.size()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.4 version=1 rust_sha256=1e2b27c5b066b1eee90e27f8bd2b752ad2efd6aa73f8ad32f301e3400210cc7d*/
struct ShardingPolicySet;

struct ShardingPolicySet {
    uint64_t version;
    int32_t num_shards;
    std::map<std::string, TableShardingPolicy> policies;

    static ShardingPolicySet with_shards(int32_t shards);
    const TableShardingPolicy* get_policy(const std::string& table_name) const;
    void set_policy(const std::string& table_name, const TableShardingPolicy& policy);
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;
    bool has_policy(const std::string& table_name) const;
    size_t table_count() const;
};


inline ShardingPolicySet ShardingPolicySet::with_shards(int32_t shards) {
    // @unsafe
    {
        return sps_with_shards(std::move(shards));
    }
}

inline const TableShardingPolicy* ShardingPolicySet::get_policy(const std::string& table_name) const {
    // @unsafe
    {
        return sps_get_policy(&this->policies, table_name);
    }
}

inline void ShardingPolicySet::set_policy(const std::string& table_name, const TableShardingPolicy& policy) {
    // @unsafe
    {
        sps_set_policy(&this->policies, table_name, policy);
    }
}

inline int32_t ShardingPolicySet::get_shard_for_key(const std::string& table_name, int64_t key_value) const {
    // @unsafe
    {
        return sps_get_shard_for_key(&this->policies, table_name, std::move(key_value));
    }
}

inline bool ShardingPolicySet::has_policy(const std::string& table_name) const {
    // @unsafe
    {
        return sps_has_policy(&this->policies, table_name);
    }
}

inline size_t ShardingPolicySet::table_count() const {
    return ((*this)).policies.size();
}
/*RUSTYCPP:GEN-END id=sharding_policy.4*/

// @safe - factory body (ShardingPolicySet is a complete type here)
inline ShardingPolicySet sps_with_shards(int32_t shards) {
    ShardingPolicySet s{};
    s.num_shards = shards;
    return s;
}

}  // namespace janus
