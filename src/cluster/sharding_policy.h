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

module;

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <rusty/array.hpp>   // c529cd3d: BTreeMap::len() free-fn decl
#include <rusty/vec.hpp>            // native-API vector (replaces std::vector)

// deref_if_pointer_like + rusty::clone, used by the DSL-generated method
// bodies below (clone wraps the enum literals in the factory struct
// literals). rusty, NOT srpc — the standalone cluster test build already
// has this include path (sharding_policy_cache.h pulls rusty too), so it
// does not break the no-srpc guarantee.
#include <rusty/slice.hpp>
#include <rusty/move.hpp>

export module cluster:sharding_policy;
import btree_port.btree.map;
import rusty;                  // c529cd3d: rusty::Vec is a module now   // c529cd3d: btree_port is now a C++20 module (retired the .hpp header)

// The policy value types serialize via their srpc Serializable save()/load()
// DSL methods (see each `impl` below) instead of free-function srpc::Marshal
// operator<< / operator>>. srpc::BinaryWrite/ReadArchive come from the
// srpc.serializable C++23 module, imported here in the module purview (an
// import cannot live in the global module fragment above) — a deliberate
// trade to keep serialization authored in the DSL.
import srpc.serializable;

namespace btree_port { using btree::map::BTreeMap; }  // compat: flat name the DSL/GEN expect

export namespace janus {

// @unsafe - deserialize into a bare local. The inline-Rust DSL moves bare-local
// by-value args, but srpc::Deserialize_::deserialize needs a T& lvalue; `&mut x`
// lowers to a raw pointer, which we forward through a deref (a place expression).
template <typename T>
inline void cluster_deser_(T* out, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(*out, ar);  // @unsafe
}


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
 * by_field/by_prefix/by_hash factories.
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
    fn by_field(index: i32) -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::FIELD_INDEX, field_index: index, prefix_length: 0 }
    }
    fn by_prefix(length: i32) -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::PREFIX_BYTES, field_index: 0, prefix_length: length }
    }
    fn by_hash() -> KeyExtractor {
        KeyExtractor { kind: KeyExtractorType::HASH_MOD, field_index: 0, prefix_length: 0 }
    }
    // srpc Serializable value contract (save/load; no polymorphic kind()).
    fn save(&self, ar: &mut srpc::BinaryWriteArchive) {
        srpc::Serialize_::serialize(((*self).kind as i32), ar);
        srpc::Serialize_::serialize((*self).field_index, ar);
        srpc::Serialize_::serialize((*self).prefix_length, ar);
    }
    fn load(&mut self, ar: &mut srpc::BinaryReadArchive) {
        let mut k: i32 = 0;
        cluster_deser_(&mut k, ar);
        (*self).kind = k as KeyExtractorType;
        srpc::Deserialize_::deserialize((*self).field_index, ar);
        srpc::Deserialize_::deserialize((*self).prefix_length, ar);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.1 version=1 rust_sha256=bcc0f26782e14c1877316ef78e2dea76a98dc12c68a064af9be6512bbc8e79d3*/
struct KeyExtractor;

struct KeyExtractor {
    KeyExtractorType kind;
    int32_t field_index;
    int32_t prefix_length;

    static KeyExtractor defaults();
    static KeyExtractor make(KeyExtractorType kind, int32_t field, int32_t prefix);
    static KeyExtractor by_field(int32_t index);
    static KeyExtractor by_prefix(int32_t length);
    static KeyExtractor by_hash();
    void save(srpc::BinaryWriteArchive& ar) const;
    void load(srpc::BinaryReadArchive& ar);
};


KeyExtractor KeyExtractor::defaults() {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::FIELD_INDEX)), .field_index = static_cast<int32_t>(0), .prefix_length = static_cast<int32_t>(4)};
}

KeyExtractor KeyExtractor::make(KeyExtractorType kind, int32_t field, int32_t prefix) {
    return KeyExtractor{.kind = std::move(kind), .field_index = std::move(field), .prefix_length = std::move(prefix)};
}

KeyExtractor KeyExtractor::by_field(int32_t index) {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::FIELD_INDEX)), .field_index = std::move(index), .prefix_length = static_cast<int32_t>(0)};
}

KeyExtractor KeyExtractor::by_prefix(int32_t length) {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::PREFIX_BYTES)), .field_index = static_cast<int32_t>(0), .prefix_length = std::move(length)};
}

KeyExtractor KeyExtractor::by_hash() {
    return KeyExtractor{.kind = rusty::clone(rusty::clone(KeyExtractorType::HASH_MOD)), .field_index = static_cast<int32_t>(0), .prefix_length = static_cast<int32_t>(0)};
}

void KeyExtractor::save(srpc::BinaryWriteArchive& ar) const {
    srpc::Serialize_::serialize((static_cast<int32_t>(((*this)).kind)), ar);
    srpc::Serialize_::serialize(((*this)).field_index, ar);
    srpc::Serialize_::serialize(((*this)).prefix_length, ar);
}

void KeyExtractor::load(srpc::BinaryReadArchive& ar) {
    int32_t k = static_cast<int32_t>(0);
    cluster_deser_(&k, ar);
    ((*this)).kind = static_cast<KeyExtractorType>(k);
    srpc::Deserialize_::deserialize(((*this)).field_index, ar);
    srpc::Deserialize_::deserialize(((*this)).prefix_length, ar);
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
    fn save(&self, ar: &mut srpc::BinaryWriteArchive) {
        srpc::Serialize_::serialize((*self).start_key, ar);
        srpc::Serialize_::serialize((*self).end_key, ar);
        srpc::Serialize_::serialize((*self).shard_id, ar);
    }
    fn load(&mut self, ar: &mut srpc::BinaryReadArchive) {
        srpc::Deserialize_::deserialize((*self).start_key, ar);
        srpc::Deserialize_::deserialize((*self).end_key, ar);
        srpc::Deserialize_::deserialize((*self).shard_id, ar);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.2 version=1 rust_sha256=08118811e85ebbaba5f5f1dda1951897ffafaed688d5e7f088f7a8b77305665d*/
struct RangeMapping;

struct RangeMapping {
    int64_t start_key;
    int64_t end_key;
    int32_t shard_id;

    static RangeMapping make(int64_t start, int64_t end, int32_t shard);
    bool contains(int64_t key) const;
    void save(srpc::BinaryWriteArchive& ar) const;
    void load(srpc::BinaryReadArchive& ar);
};


RangeMapping RangeMapping::make(int64_t start, int64_t end, int32_t shard) {
    return RangeMapping{.start_key = std::move(start), .end_key = std::move(end), .shard_id = std::move(shard)};
}

bool RangeMapping::contains(int64_t key) const {
    return (rusty::detail::deref_if_pointer_like(((*this)).start_key) <= rusty::detail::deref_if_pointer_like(key)) && (rusty::detail::deref_if_pointer_like(key) < rusty::detail::deref_if_pointer_like(((*this)).end_key));
}

void RangeMapping::save(srpc::BinaryWriteArchive& ar) const {
    srpc::Serialize_::serialize(((*this)).start_key, ar);
    srpc::Serialize_::serialize(((*this)).end_key, ar);
    srpc::Serialize_::serialize(((*this)).shard_id, ar);
}

void RangeMapping::load(srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(((*this)).start_key, ar);
    srpc::Deserialize_::deserialize(((*this)).end_key, ar);
    srpc::Deserialize_::deserialize(((*this)).shard_id, ar);
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
    // srpc Serializable: name, extractor, range count + ranges, default_shard.
    fn save(&self, ar: &mut srpc::BinaryWriteArchive) {
        srpc::Serialize_::serialize((*self).table_name, ar);
        (*self).key_extractor.save(ar);
        srpc::Serialize_::serialize(((*self).ranges.size() as i32), ar);
        let mut i: usize = 0;
        while i < (*self).ranges.size() {
            (*self).ranges[i].save(ar);
            i = i + 1;
        }
        srpc::Serialize_::serialize((*self).default_shard, ar);
    }
    fn load(&mut self, ar: &mut srpc::BinaryReadArchive) {
        srpc::Deserialize_::deserialize((*self).table_name, ar);
        (*self).key_extractor.load(ar);
        let mut n: i32 = 0;
        cluster_deser_(&mut n, ar);
        (*self).ranges.clear();
        let mut i: i32 = 0;
        while i < n {
            let mut r: RangeMapping = RangeMapping::make(0, 0, 0);
            r.load(ar);
            (*self).ranges.push(r);
            i = i + 1;
        }
        srpc::Deserialize_::deserialize((*self).default_shard, ar);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.3 version=1 rust_sha256=d3f26d2b3cfb58409f2600a296752430c7b9ee61d1b2224b267331f63d0a7377*/
struct TableShardingPolicy;

struct TableShardingPolicy {
    std::string table_name;
    KeyExtractor key_extractor;
    rusty::Vec<RangeMapping> ranges;
    int32_t default_shard;

    static TableShardingPolicy create(const std::string& name, const KeyExtractor& extractor);
    int32_t get_shard(int64_t key_value) const;
    void add_range(int64_t start, int64_t end, int32_t shard);
    void save(srpc::BinaryWriteArchive& ar) const;
    void load(srpc::BinaryReadArchive& ar);
};


TableShardingPolicy TableShardingPolicy::create(const std::string& name, const KeyExtractor& extractor) {
    return TableShardingPolicy{.table_name = name, .key_extractor = extractor, .ranges = rusty::Vec<RangeMapping>::new_(), .default_shard = -1};
}

int32_t TableShardingPolicy::get_shard(int64_t key_value) const {
    int32_t left = static_cast<int32_t>(0);
    int32_t right = ((static_cast<int32_t>(((*this)).ranges.size()))) - static_cast<int32_t>(1);
    while (rusty::detail::deref_if_pointer_like(left) <= rusty::detail::deref_if_pointer_like(right)) {
        const int32_t mid = rusty::detail::deref_if_pointer_like(left) + (((rusty::detail::deref_if_pointer_like(right) - rusty::detail::deref_if_pointer_like(left))) / static_cast<int32_t>(2));
        if (rusty::detail::deref_if_pointer_like(key_value) < rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.start_key); }) { return (__r.start_key); } else if constexpr (requires { (__r.start_key_field); }) { return (__r.start_key_field); } else if constexpr (requires { ((*__r).start_key); }) { return ((*__r).start_key); } else { return ((*__r).start_key_field); } }(((*this)).ranges[mid]))) {
            right = rusty::detail::deref_if_pointer_like(mid) - static_cast<int32_t>(1);
        } else if (rusty::detail::deref_if_pointer_like(key_value) >= rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.end_key); }) { return (__r.end_key); } else if constexpr (requires { (__r.end_key_field); }) { return (__r.end_key_field); } else if constexpr (requires { ((*__r).end_key); }) { return ((*__r).end_key); } else { return ((*__r).end_key_field); } }(((*this)).ranges[mid]))) {
            left = rusty::detail::deref_if_pointer_like(mid) + static_cast<int32_t>(1);
        } else {
            return [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.shard_id); }) { return (__r.shard_id); } else if constexpr (requires { (__r.shard_id_field); }) { return (__r.shard_id_field); } else if constexpr (requires { ((*__r).shard_id); }) { return ((*__r).shard_id); } else { return ((*__r).shard_id_field); } }(((*this)).ranges[mid]);
        }
    }
    return ((*this)).default_shard;
}

void TableShardingPolicy::add_range(int64_t start, int64_t end, int32_t shard) {
    RangeMapping mapping = RangeMapping::make(std::move(start), std::move(end), std::move(shard));
    size_t idx = static_cast<size_t>(0);
    while ((rusty::detail::deref_if_pointer_like(idx) < ((*this)).ranges.size()) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.start_key); }) { return (__r.start_key); } else if constexpr (requires { (__r.start_key_field); }) { return (__r.start_key_field); } else if constexpr (requires { ((*__r).start_key); }) { return ((*__r).start_key); } else { return ((*__r).start_key_field); } }(((*this)).ranges[idx])) < rusty::detail::deref_if_pointer_like(start))) {
        idx = rusty::detail::deref_if_pointer_like(idx) + static_cast<size_t>(1);
    }
    ((*this)).ranges.insert(std::move(idx), std::move(mapping));
}

void TableShardingPolicy::save(srpc::BinaryWriteArchive& ar) const {
    srpc::Serialize_::serialize(((*this)).table_name, ar);
    ((*this)).key_extractor.save(ar);
    srpc::Serialize_::serialize((static_cast<int32_t>(((*this)).ranges.size())), ar);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).ranges.size()) {
        ((*this)).ranges[i].save(ar);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    srpc::Serialize_::serialize(((*this)).default_shard, ar);
}

void TableShardingPolicy::load(srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(((*this)).table_name, ar);
    ((*this)).key_extractor.load(ar);
    int32_t n = static_cast<int32_t>(0);
    cluster_deser_(&n, ar);
    ((*this)).ranges.clear();
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        RangeMapping r = RangeMapping::make(0, 0, 0);
        r.load(ar);
        ((*this)).ranges.push(std::move(r));
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<int32_t>(1);
    }
    srpc::Deserialize_::deserialize(((*this)).default_shard, ar);
}
/*RUSTYCPP:GEN-END id=sharding_policy.3*/

/**
 * Complete sharding policy set containing all table policies.
 */
// C++ kernels for the ShardingPolicySet map lookups below (iterator /
// pointer surgery the DSL should not hand-roll). with_shards is the factory
// replacing the (num_shards) constructor.
struct ShardingPolicySet;  // for the factory kernel's forward declaration

// A borrow of a table's policy, or None -- the natural return of
// btree_port::BTreeMap::get(). Aliased so the DSL can name it with a single
// identifier (the transpiler does not parse `const T` inside generic args).
// get_policy() now forwards the map's Option<&V> directly: no raw pointer,
// no kernel.
using TableShardingPolicyRef =
    rusty::Option<const TableShardingPolicy&>;

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
    // Borrow of a table's policy, or None. Forwards btree_port::BTreeMap::get
    // (Option<&V>) straight through -- no raw pointer, no kernel.
    fn get_policy(&self, table_name: &std::string) -> TableShardingPolicyRef {
        (*self).policies.get(table_name)
    }
    // Add or overwrite a table's policy (BTreeMap::insert overwrites).
    fn set_policy(&mut self, table_name: &std::string, policy: &TableShardingPolicy) {
        (*self).policies.insert(table_name, policy);
    }
    // Route: shard for (table, key), or -1 if the table has no policy.
    fn get_shard_for_key(&self, table_name: &std::string, key_value: i64) -> i32 {
        if (*self).policies.contains_key(table_name) {
            return (*self).policies.get(table_name).unwrap().get_shard(key_value);
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
        (*self).policies.len()
    }
    // srpc Serializable: version, num_shards, then each policy (the key is
    // rebuilt from policy.table_name on load, so only values are written).
    fn save(&self, ar: &mut srpc::BinaryWriteArchive) {
        srpc::Serialize_::serialize((*self).version, ar);
        srpc::Serialize_::serialize((*self).num_shards, ar);
        srpc::Serialize_::serialize(((*self).policies.len() as i32), ar);
        for kv in (*self).policies {
            kv.1.save(ar);
        }
    }
    fn load(&mut self, ar: &mut srpc::BinaryReadArchive) {
        srpc::Deserialize_::deserialize((*self).version, ar);
        srpc::Deserialize_::deserialize((*self).num_shards, ar);
        let mut n: i32 = 0;
        cluster_deser_(&mut n, ar);
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
/*RUSTYCPP:GEN-BEGIN id=sharding_policy.4 version=1 rust_sha256=5992d91db2510c806df29680a3f8b3c76a58d6e114c8715f6c83696319a919fa*/
struct ShardingPolicySet;

struct ShardingPolicySet {
    uint64_t version;
    int32_t num_shards;
    btree_port::BTreeMap<std::string, TableShardingPolicy> policies;

    static ShardingPolicySet with_shards(int32_t shards);
    TableShardingPolicyRef get_policy(const std::string& table_name) const;
    void set_policy(const std::string& table_name, const TableShardingPolicy& policy);
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;
    bool has_policy(const std::string& table_name) const;
    size_t table_count() const;
    void save(srpc::BinaryWriteArchive& ar) const;
    void load(srpc::BinaryReadArchive& ar);
};


ShardingPolicySet ShardingPolicySet::with_shards(int32_t shards) {
    return ShardingPolicySet{.version = static_cast<uint64_t>(0), .num_shards = std::move(shards), .policies = btree_port::BTreeMap<std::string, TableShardingPolicy>::new_()};
}

TableShardingPolicyRef ShardingPolicySet::get_policy(const std::string& table_name) const {
    return ((*this)).policies.get(table_name);
}

void ShardingPolicySet::set_policy(const std::string& table_name, const TableShardingPolicy& policy) {
    ((*this)).policies.insert(table_name, std::move(policy));
}

int32_t ShardingPolicySet::get_shard_for_key(const std::string& table_name, int64_t key_value) const {
    if (((*this)).policies.contains_key(table_name)) {
        return ((*this)).policies.get(table_name).unwrap().get_shard(std::move(key_value));
    }
    return -1;
}

bool ShardingPolicySet::has_policy(const std::string& table_name) const {
    return ((*this)).policies.contains_key(table_name);
}

size_t ShardingPolicySet::table_count() const {
    return rusty::len(((*this)).policies);
}

void ShardingPolicySet::save(srpc::BinaryWriteArchive& ar) const {
    srpc::Serialize_::serialize(((*this)).version, ar);
    srpc::Serialize_::serialize(((*this)).num_shards, ar);
    srpc::Serialize_::serialize((static_cast<int32_t>(rusty::len(((*this)).policies))), ar);
    for (auto&& kv : rusty::for_in(((*this)).policies)) {
        rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv)).save(ar);
    }
}

void ShardingPolicySet::load(srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(((*this)).version, ar);
    srpc::Deserialize_::deserialize(((*this)).num_shards, ar);
    int32_t n = static_cast<int32_t>(0);
    cluster_deser_(&n, ar);
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
