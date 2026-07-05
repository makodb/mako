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
#include <btree_port/btreemap.hpp>  // native-API ordered map (replaces std::map)
#include <rusty/vec.hpp>            // native-API vector (replaces std::vector)

// deref_if_pointer_like + rusty::clone, used by the DSL-generated method
// bodies below (clone wraps the enum literals in the factory struct
// literals). rusty, NOT rrr — the standalone cluster test build already
// has this include path (sharding_policy_cache.h pulls rusty too), so it
// does not break the no-rrr guarantee.
#include <rusty/slice.hpp>
#include <rusty/move.hpp>
#include "rrr/misc/serializable.hpp"   // rrr Serializable (BinaryWrite/ReadArchive)

// NOTE: the policy value types serialize via their rrr Serializable
// save()/load() DSL methods (see each `impl` below) instead of free-function
// rrr::Marshal operator<< / operator>>. That pulls the rrr.serializable
// module into this header (so cluster_config.h / test_config_manager now
// link rrr) — a deliberate trade to keep serialization authored in the DSL
// rather than as C++ operator overloads the transpiler can't emit.

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
    // rrr Serializable value contract (save/load; no polymorphic kind()).
    fn save(&self, ar: &mut rrr::BinaryWriteArchive) {
        ar << ((*self).kind as i32);
        ar << (*self).field_index;
        ar << (*self).prefix_length;
    }
    fn load(&mut self, ar: &mut rrr::BinaryReadArchive) {
        let mut k: i32 = 0;
        ar >> k;
        (*self).kind = k as KeyExtractorType;
        ar >> (*self).field_index;
        ar >> (*self).prefix_length;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.1 version=1 rust_sha256=b7d60778e08800a81dd0a51bbbbb9767de47a4429330c404f705c4110d9667b4*/
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
    void save(rrr::BinaryWriteArchive& ar) const;
    void load(rrr::BinaryReadArchive& ar);
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

inline void KeyExtractor::save(rrr::BinaryWriteArchive& ar) const {
    rusty::detail::deref_if_pointer_like(ar) << ((static_cast<int32_t>(((*this)).kind)));
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).field_index);
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).prefix_length);
}

inline void KeyExtractor::load(rrr::BinaryReadArchive& ar) {
    int32_t k = static_cast<int32_t>(0);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(k);
    ((*this)).kind = static_cast<KeyExtractorType>(k);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).field_index);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).prefix_length);
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
    fn save(&self, ar: &mut rrr::BinaryWriteArchive) {
        ar << (*self).start_key;
        ar << (*self).end_key;
        ar << (*self).shard_id;
    }
    fn load(&mut self, ar: &mut rrr::BinaryReadArchive) {
        ar >> (*self).start_key;
        ar >> (*self).end_key;
        ar >> (*self).shard_id;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.2 version=1 rust_sha256=5e81f5ff8d2bca5b747994799be87be944b1b5fe603c6f6c4e6c9adce2e57352*/
struct RangeMapping;

struct RangeMapping {
    int64_t start_key;
    int64_t end_key;
    int32_t shard_id;

    static RangeMapping make(int64_t start, int64_t end, int32_t shard);
    bool contains(int64_t key) const;
    void save(rrr::BinaryWriteArchive& ar) const;
    void load(rrr::BinaryReadArchive& ar);
};


inline RangeMapping RangeMapping::make(int64_t start, int64_t end, int32_t shard) {
    return RangeMapping{.start_key = std::move(start), .end_key = std::move(end), .shard_id = std::move(shard)};
}

inline bool RangeMapping::contains(int64_t key) const {
    return (rusty::detail::deref_if_pointer_like(((*this)).start_key) <= rusty::detail::deref_if_pointer_like(key)) && (rusty::detail::deref_if_pointer_like(key) < rusty::detail::deref_if_pointer_like(((*this)).end_key));
}

inline void RangeMapping::save(rrr::BinaryWriteArchive& ar) const {
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).start_key);
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).end_key);
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).shard_id);
}

inline void RangeMapping::load(rrr::BinaryReadArchive& ar) {
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).start_key);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).end_key);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).shard_id);
}
/*RUSTYCPP:GEN-END id=sharding_policy.2*/

/**
 * Sharding policy for a single table.
 */
// TableShardingPolicy is fully DSL now (no C++ kernels): ranges is a
// rusty::Vec<RangeMapping>, so create (struct literal with Vec::new_()),
// get_shard (binary search), and add_range (sorted insert via
// Vec::insert(index, value)) all live in the DSL bodies below.

#if RUSTYCPP_RUST
pub struct TableShardingPolicy {
    table_name: std::string,
    key_extractor: KeyExtractor,
    ranges: rusty::Vec<RangeMapping>,    // Sorted by start_key for binary search
    default_shard: i32,                  // -1 means error if no range matches
}
impl TableShardingPolicy {
    // Factory replacing the old (name, extractor) constructor. Pure-DSL
    // struct literal now that ranges is a rusty::Vec (has new_()).
    fn create(name: &std::string, extractor: &KeyExtractor) -> TableShardingPolicy {
        TableShardingPolicy {
            table_name: name,
            key_extractor: extractor,
            ranges: rusty::Vec::<RangeMapping>::new_(),
            default_shard: -1,
        }
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
    // Insert a range, keeping ranges sorted by start_key (Vec::insert by
    // index — the old tsp_add_range_sorted iterator kernel, now pure DSL).
    fn add_range(&mut self, start: i64, end: i64, shard: i32) {
        let mapping: RangeMapping = RangeMapping::make(start, end, shard);
        let mut idx: usize = 0;
        while idx < (*self).ranges.size() && (*self).ranges[idx].start_key < start {
            idx = idx + 1;
        }
        (*self).ranges.insert(idx, mapping);
    }
    // rrr Serializable: name, extractor, range count + ranges, default_shard.
    fn save(&self, ar: &mut rrr::BinaryWriteArchive) {
        ar << (*self).table_name;
        (*self).key_extractor.save(ar);
        ar << ((*self).ranges.size() as i32);
        let mut i: usize = 0;
        while i < (*self).ranges.size() {
            (*self).ranges[i].save(ar);
            i = i + 1;
        }
        ar << (*self).default_shard;
    }
    fn load(&mut self, ar: &mut rrr::BinaryReadArchive) {
        ar >> (*self).table_name;
        (*self).key_extractor.load(ar);
        let mut n: i32 = 0;
        ar >> n;
        (*self).ranges.clear();
        let mut i: i32 = 0;
        while i < n {
            let mut r: RangeMapping = RangeMapping::make(0, 0, 0);
            r.load(ar);
            (*self).ranges.push(r);
            i = i + 1;
        }
        ar >> (*self).default_shard;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.3 version=1 rust_sha256=356bf8820f6e82baa2a21ef1678718230cd931231cad2d6ae9b4ae726b276329*/
struct TableShardingPolicy;

struct TableShardingPolicy {
    std::string table_name;
    KeyExtractor key_extractor;
    rusty::Vec<RangeMapping> ranges;
    int32_t default_shard;

    static TableShardingPolicy create(const std::string& name, const KeyExtractor& extractor);
    int32_t get_shard(int64_t key_value) const;
    void add_range(int64_t start, int64_t end, int32_t shard);
    void save(rrr::BinaryWriteArchive& ar) const;
    void load(rrr::BinaryReadArchive& ar);
};


inline TableShardingPolicy TableShardingPolicy::create(const std::string& name, const KeyExtractor& extractor) {
    return TableShardingPolicy{.table_name = name, .key_extractor = extractor, .ranges = rusty::Vec<RangeMapping>::new_(), .default_shard = -1};
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
    RangeMapping mapping = RangeMapping::make(std::move(start), std::move(end), std::move(shard));
    size_t idx = static_cast<size_t>(0);
    while ((rusty::detail::deref_if_pointer_like(idx) < ((*this)).ranges.size()) && (rusty::detail::deref_if_pointer_like(((*this)).ranges[idx].start_key) < rusty::detail::deref_if_pointer_like(start))) {
        idx = rusty::detail::deref_if_pointer_like(idx) + static_cast<size_t>(1);
    }
    ((*this)).ranges.insert(std::move(idx), std::move(mapping));
}

inline void TableShardingPolicy::save(rrr::BinaryWriteArchive& ar) const {
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).table_name);
    ((*this)).key_extractor.save(ar);
    rusty::detail::deref_if_pointer_like(ar) << ((static_cast<int32_t>(((*this)).ranges.size())));
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).ranges.size()) {
        ((*this)).ranges[i].save(ar);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).default_shard);
}

inline void TableShardingPolicy::load(rrr::BinaryReadArchive& ar) {
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).table_name);
    ((*this)).key_extractor.load(ar);
    int32_t n = static_cast<int32_t>(0);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(n);
    ((*this)).ranges.clear();
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        RangeMapping r = RangeMapping::make(0, 0, 0);
        r.load(ar);
        ((*this)).ranges.push(std::move(r));
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<int32_t>(1);
    }
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).default_shard);
}
/*RUSTYCPP:GEN-END id=sharding_policy.3*/

/**
 * Complete sharding policy set containing all table policies.
 */
// C++ kernels for the ShardingPolicySet map lookups below (iterator /
// pointer surgery the DSL should not hand-roll). with_shards is the factory
// replacing the (num_shards) constructor.
struct ShardingPolicySet;  // for the factory kernel's forward declaration

// @unsafe - BTreeMap get -> pointer-or-null. The one map op that stays a
// kernel: it hands back a raw pointer into the map (the DSL should not
// hand-roll pointer surgery). insert / contains_key / get-and-route now
// live in the DSL bodies below as btree_port::BTreeMap method calls.
inline const TableShardingPolicy* sps_get_policy(
        const btree_port::BTreeMap<std::string, TableShardingPolicy>* policies,
        const std::string& table_name) {
    auto found = policies->get(table_name);
    return found.is_some() ? &found.unwrap().get() : nullptr;
}

#if RUSTYCPP_RUST
pub struct ShardingPolicySet {
    version: u64,                                                       // cache-invalidation version
    num_shards: i32,                                                    // total shards in the cluster
    policies: btree_port::BTreeMap<std::string, TableShardingPolicy>,   // table_name -> policy
}
impl ShardingPolicySet {
    // Factory replacing the old (shards) constructor. Pure-DSL struct
    // literal now that policies is a btree_port::BTreeMap with new_().
    fn with_shards(shards: i32) -> ShardingPolicySet {
        ShardingPolicySet {
            version: 0,
            num_shards: shards,
            policies: btree_port::BTreeMap::<std::string, TableShardingPolicy>::new_(),
        }
    }
    // Policy for a table, or null if none is registered. The raw-pointer
    // return keeps this one op a kernel (see sps_get_policy).
    fn get_policy(&self, table_name: &std::string) -> *const TableShardingPolicy {
        unsafe { sps_get_policy(&self.policies, table_name) }
    }
    // Add or overwrite a table's policy (BTreeMap::insert overwrites).
    fn set_policy(&mut self, table_name: &std::string, policy: &TableShardingPolicy) {
        (*self).policies.insert(table_name, policy);
    }
    // Route: shard for (table, key), or -1 if the table has no policy.
    fn get_shard_for_key(&self, table_name: &std::string, key_value: i64) -> i32 {
        if (*self).policies.contains_key(table_name) {
            return (*self).policies.get(table_name).unwrap().get().get_shard(key_value);
        }
        -1
    }
    // True if a policy is registered for the table.
    fn has_policy(&self, table_name: &std::string) -> bool {
        (*self).policies.contains_key(table_name)
    }
    // Number of tables with a policy. (.size() lowers directly; .len()
    // would map to rusty::len, which this pin doesn't provide.)
    fn table_count(&self) -> usize {
        (*self).policies.size()
    }
    // rrr Serializable: version, num_shards, then each policy (the key is
    // rebuilt from policy.table_name on load, so only values are written).
    fn save(&self, ar: &mut rrr::BinaryWriteArchive) {
        ar << (*self).version;
        ar << (*self).num_shards;
        ar << ((*self).policies.size() as i32);
        for kv in (*self).policies {
            kv.second.save(ar);
        }
    }
    fn load(&mut self, ar: &mut rrr::BinaryReadArchive) {
        ar >> (*self).version;
        ar >> (*self).num_shards;
        let mut n: i32 = 0;
        ar >> n;
        (*self).policies.clear();
        let empty: std::string = std::string();
        let ext: KeyExtractor = KeyExtractor::defaults();
        let ename: &std::string = &empty;
        let mut i: i32 = 0;
        while i < n {
            let mut p: TableShardingPolicy = TableShardingPolicy::create(ename, &ext);
            p.load(ar);
            let key: std::string = p.table_name;
            (*self).policies.insert(key, p);
            i = i + 1;
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.4 version=1 rust_sha256=01643e353bdf2aedd6f667ab86919c950b1b432be9932f59e3f042279b1d3299*/
struct ShardingPolicySet;

struct ShardingPolicySet {
    uint64_t version;
    int32_t num_shards;
    btree_port::BTreeMap<std::string, TableShardingPolicy> policies;

    static ShardingPolicySet with_shards(int32_t shards);
    const TableShardingPolicy* get_policy(const std::string& table_name) const;
    void set_policy(const std::string& table_name, const TableShardingPolicy& policy);
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;
    bool has_policy(const std::string& table_name) const;
    size_t table_count() const;
    void save(rrr::BinaryWriteArchive& ar) const;
    void load(rrr::BinaryReadArchive& ar);
};


inline ShardingPolicySet ShardingPolicySet::with_shards(int32_t shards) {
    return ShardingPolicySet{.version = static_cast<uint64_t>(0), .num_shards = std::move(shards), .policies = btree_port::BTreeMap<std::string, TableShardingPolicy>::new_()};
}

inline const TableShardingPolicy* ShardingPolicySet::get_policy(const std::string& table_name) const {
    // @unsafe
    {
        return sps_get_policy(&this->policies, table_name);
    }
}

inline void ShardingPolicySet::set_policy(const std::string& table_name, const TableShardingPolicy& policy) {
    ((*this)).policies.insert(table_name, std::move(policy));
}

inline int32_t ShardingPolicySet::get_shard_for_key(const std::string& table_name, int64_t key_value) const {
    if (((*this)).policies.contains_key(table_name)) {
        return ((*this)).policies.get(table_name).unwrap().get().get_shard(std::move(key_value));
    }
    return -1;
}

inline bool ShardingPolicySet::has_policy(const std::string& table_name) const {
    return ((*this)).policies.contains_key(table_name);
}

inline size_t ShardingPolicySet::table_count() const {
    return ((*this)).policies.size();
}

inline void ShardingPolicySet::save(rrr::BinaryWriteArchive& ar) const {
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).version);
    rusty::detail::deref_if_pointer_like(ar) << rusty::detail::deref_if_pointer_like(((*this)).num_shards);
    rusty::detail::deref_if_pointer_like(ar) << ((static_cast<int32_t>(((*this)).policies.size())));
    for (auto&& kv : rusty::for_in(((*this)).policies)) {
        kv.second.save(ar);
    }
}

inline void ShardingPolicySet::load(rrr::BinaryReadArchive& ar) {
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).version);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(((*this)).num_shards);
    int32_t n = static_cast<int32_t>(0);
    rusty::detail::deref_if_pointer_like(ar) >> rusty::detail::deref_if_pointer_like(n);
    ((*this)).policies.clear();
    const std::string empty = std::string();
    const KeyExtractor ext = KeyExtractor::defaults();
    const std::string& ename = empty;
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        TableShardingPolicy p = TableShardingPolicy::create(ename, ext);
        p.load(ar);
        const std::string key = p.table_name;
        ((*this)).policies.insert(std::move(key), std::move(p));
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<int32_t>(1);
    }
}
/*RUSTYCPP:GEN-END id=sharding_policy.4*/


}  // namespace janus
