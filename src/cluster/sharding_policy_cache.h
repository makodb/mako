#pragma once

/**
 * @file sharding_policy_cache.h
 * @brief Client-side cache for sharding policy with routing functions.
 *
 * Local cache of a ShardingPolicySet plus routing helpers. Thread-safe:
 * the policy lives behind a rusty::Mutex and the version/init flags are
 * rusty::Cell. Authored in the inline-Rust DSL (docs/storage-interface.md):
 * the `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
 * scripts/regen_storage_dsl.sh. The mutex lock/guard, the Cell accessors,
 * and the Option handling all lower cleanly; only the two operations that
 * are pointer / raw-byte surgery stay as C++ kernels the DSL body calls
 * (composite-key routing through a get_policy() pointer, and the
 * PREFIX_BYTES/HASH_MOD decode over a const char*).
 */

#include "sharding_policy.h"
#include <rusty/option.hpp>
#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (guard/pointer bodies)
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace janus {

#if RUSTYCPP_RUST
pub struct ShardingPolicyCache {
    policy: rusty::Mutex<rusty::Option<ShardingPolicySet>>,
    cached_version: rusty::Cell<u64>,
    initialized: rusty::Cell<bool>,
}
impl ShardingPolicyCache {
    // Replaces the old default constructor. Pure-DSL struct literal: every
    // field type has a Rust-style constructor callable via turbofish
    // (Mutex::default_ default-constructs the Option to None; Cell::new_).
    fn new() -> ShardingPolicyCache {
        ShardingPolicyCache {
            policy: rusty::Mutex::<rusty::Option<ShardingPolicySet>>::default_(),
            cached_version: rusty::Cell::<u64>::new_(0),
            initialized: rusty::Cell::<bool>::new_(false),
        }
    }
    // Set the policy directly (offline init / tests).
    fn set_policy(&mut self, policy: ShardingPolicySet) {
        let version: u64 = policy.version;
        let mut guard = (*self).policy.lock().unwrap();
        *guard = rusty::Some(policy);
        (*self).cached_version.set(version);
        (*self).initialized.set(true);
    }
    fn is_initialized(&self) -> bool {
        (*self).initialized.get()
    }
    fn clear(&mut self) {
        let mut guard = (*self).policy.lock().unwrap();
        *guard = rusty::None;
        (*self).cached_version.set(0);
        (*self).initialized.set(false);
    }
    fn get_version(&self) -> u64 {
        (*self).cached_version.get()
    }
    // Route a table + already-extracted key value.
    fn get_shard_for_key(&self, table_name: &std::string, key_value: i64) -> i32 {
        if !(*self).initialized.get() { return -1; }
        let guard = (*self).policy.lock().unwrap();
        if (*guard).is_none() { return -1; }
        (*guard).as_ref().unwrap().get_shard_for_key(table_name, key_value)
    }
    // Route a composite key: find the table's policy, extract its key with
    // that policy's extractor, then route. All DSL now -- get_policy hands
    // back an Option<&policy> (no raw pointer) and extract_key_value is our
    // own sibling, so the old spc_composite_route kernel is gone.
    fn get_shard_for_composite_key(&self, table_name: &std::string, key_fields: &std::vector<i64>) -> i32 {
        if !(*self).initialized.get() { return -1; }
        let guard = (*self).policy.lock().unwrap();
        if (*guard).is_none() { return -1; }
        let found = (*guard).as_ref().unwrap().get_policy(table_name);
        if found.is_none() { return -1; }
        let tp = found.unwrap().get();
        let ext: KeyExtractor = tp.key_extractor;
        let key_value: i64 = ShardingPolicyCache::extract_key_value(ext, key_fields);
        if key_value < 0 { return tp.default_shard; }
        tp.get_shard(key_value)
    }
    fn has_policy_for_table(&self, table_name: &std::string) -> bool {
        if !(*self).initialized.get() { return false; }
        let guard = (*self).policy.lock().unwrap();
        if (*guard).is_none() { return false; }
        (*guard).as_ref().unwrap().has_policy(table_name)
    }
    fn get_num_shards(&self) -> i32 {
        if !(*self).initialized.get() { return 0; }
        let guard = (*self).policy.lock().unwrap();
        if (*guard).is_none() { return 0; }
        (*guard).as_ref().unwrap().num_shards
    }
    // Extract a key value from composite int64 fields.
    fn extract_key_value(extractor: &KeyExtractor, key_fields: &std::vector<i64>) -> i64 {
        if (*extractor).kind == KeyExtractorType::FIELD_INDEX {
            let field_index: i32 = (*extractor).field_index;
            if field_index < 0 || (field_index as usize) >= (*key_fields).size() {
                return -1;
            }
            return (*key_fields)[field_index];
        }
        if (*extractor).kind == KeyExtractorType::HASH_MOD {
            let mut hash: i64 = 0;
            let mut i: usize = 0;
            while i < (*key_fields).size() {
                hash = hash ^ (*key_fields)[i];
                hash = (hash << 7) | (hash >> 57);
                i = i + 1;
            }
            if hash < 0 { return -hash; }
            return hash;
        }
        -1
    }
    // Extract a key value from raw key bytes (PREFIX_BYTES / HASH_MOD). Takes
    // the bytes as a std::string (safe indexing) instead of a raw pointer, so
    // the loop is DSL -- the spc_extract_from_bytes kernel is gone.
    fn extract_key_from_bytes(extractor: &KeyExtractor, key: &std::string) -> i64 {
        if (*extractor).kind == KeyExtractorType::PREFIX_BYTES {
            let prefix_len: i32 = (*extractor).prefix_length;
            if prefix_len <= 0 || (prefix_len as usize) > (*key).size() {
                return -1;
            }
            let mut n: usize = prefix_len as usize;
            if n > 8 { n = 8; }
            let mut result: i64 = 0;
            let mut i: usize = 0;
            while i < n {
                result = (result << 8) | (((*key)[i] as u8) as i64);
                i = i + 1;
            }
            return result;
        }
        if (*extractor).kind == KeyExtractorType::HASH_MOD {
            let mut hash: i64 = 0;
            let mut i: usize = 0;
            while i < (*key).size() {
                hash = hash ^ (((*key)[i] as u8) as i64);
                hash = (hash << 7) | (hash >> 57);
                i = i + 1;
            }
            if hash < 0 { return -hash; }
            return hash;
        }
        -1
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=sharding_policy_cache.1 version=1 rust_sha256=b4693a01261a565fbdb3c2b3ece1d3378f6aa798c2782af4d0a7ec31f6152bba*/
struct ShardingPolicyCache;

struct ShardingPolicyCache {
    rusty::Mutex<rusty::Option<ShardingPolicySet>> policy;
    rusty::Cell<uint64_t> cached_version;
    rusty::Cell<bool> initialized;

    static ShardingPolicyCache new_();
    void set_policy(ShardingPolicySet policy);
    bool is_initialized() const;
    void clear();
    uint64_t get_version() const;
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;
    int32_t get_shard_for_composite_key(const std::string& table_name, const std::vector<int64_t>& key_fields) const;
    bool has_policy_for_table(const std::string& table_name) const;
    int32_t get_num_shards() const;
    static int64_t extract_key_value(const KeyExtractor& extractor, const std::vector<int64_t>& key_fields);
    static int64_t extract_key_from_bytes(const KeyExtractor& extractor, const std::string& key);
};


inline ShardingPolicyCache ShardingPolicyCache::new_() {
    return ShardingPolicyCache{.policy = rusty::Mutex<rusty::Option<ShardingPolicySet>>::default_(), .cached_version = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .initialized = rusty::Cell<bool>::new_(false)};
}

inline void ShardingPolicyCache::set_policy(ShardingPolicySet policy) {
    uint64_t version = policy.version;
    auto guard = ((*this)).policy.lock().unwrap();
    rusty::detail::deref_if_pointer_like(guard) = rusty::Option<ShardingPolicySet>(std::move(policy));
    ((*this)).cached_version.set(std::move(version));
    ((*this)).initialized.set(true);
}

inline bool ShardingPolicyCache::is_initialized() const {
    return ((*this)).initialized.get();
}

inline void ShardingPolicyCache::clear() {
    auto guard = ((*this)).policy.lock().unwrap();
    rusty::detail::deref_if_pointer_like(guard) = rusty::None;
    ((*this)).cached_version.set(0);
    ((*this)).initialized.set(false);
}

inline uint64_t ShardingPolicyCache::get_version() const {
    return ((*this)).cached_version.get();
}

inline int32_t ShardingPolicyCache::get_shard_for_key(const std::string& table_name, int64_t key_value) const {
    if (!((*this)).initialized.get()) {
        return -1;
    }
    const auto guard = ((*this)).policy.lock().unwrap();
    if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
        return -1;
    }
    return ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap().get_shard_for_key(table_name, std::move(key_value));
}

inline int32_t ShardingPolicyCache::get_shard_for_composite_key(const std::string& table_name, const std::vector<int64_t>& key_fields) const {
    if (!((*this)).initialized.get()) {
        return -1;
    }
    const auto guard = ((*this)).policy.lock().unwrap();
    if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
        return -1;
    }
    auto found = ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap().get_policy(table_name);
    if (found.is_none()) {
        return -1;
    }
    const auto tp = found.unwrap().get();
    const KeyExtractor ext = tp.key_extractor;
    const int64_t key_value = ShardingPolicyCache::extract_key_value(ext, key_fields);
    if (rusty::detail::deref_if_pointer_like(key_value) < 0) {
        return tp.default_shard;
    }
    return tp.get_shard(std::move(key_value));
}

inline bool ShardingPolicyCache::has_policy_for_table(const std::string& table_name) const {
    if (!((*this)).initialized.get()) {
        return false;
    }
    const auto guard = ((*this)).policy.lock().unwrap();
    if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
        return false;
    }
    return ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap().has_policy(table_name);
}

inline int32_t ShardingPolicyCache::get_num_shards() const {
    if (!((*this)).initialized.get()) {
        return static_cast<int32_t>(0);
    }
    const auto guard = ((*this)).policy.lock().unwrap();
    if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
        return static_cast<int32_t>(0);
    }
    return ((rusty::detail::deref_if_pointer_like(guard))).as_ref().unwrap().num_shards;
}

inline int64_t ShardingPolicyCache::extract_key_value(const KeyExtractor& extractor, const std::vector<int64_t>& key_fields) {
    if (rusty::detail::deref_if_pointer_like((extractor).kind) == rusty::clone(KeyExtractorType::FIELD_INDEX)) {
        const int32_t field_index = (extractor).field_index;
        if ((rusty::detail::deref_if_pointer_like(field_index) < 0) || (((static_cast<size_t>(field_index))) >= ((key_fields)).size())) {
            return -1;
        }
        return (key_fields)[field_index];
    }
    if (rusty::detail::deref_if_pointer_like((extractor).kind) == rusty::clone(KeyExtractorType::HASH_MOD)) {
        int64_t hash = static_cast<int64_t>(0);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < ((key_fields)).size()) {
            hash = rusty::detail::deref_if_pointer_like(hash) ^ (key_fields)[i];
            hash = ((rusty::detail::deref_if_pointer_like(hash) << 7)) | ((rusty::detail::deref_if_pointer_like(hash) >> 57));
            i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
        }
        if (rusty::detail::deref_if_pointer_like(hash) < 0) {
            return -hash;
        }
        return std::move(hash);
    }
    return -1;
}

inline int64_t ShardingPolicyCache::extract_key_from_bytes(const KeyExtractor& extractor, const std::string& key) {
    if (rusty::detail::deref_if_pointer_like((extractor).kind) == rusty::clone(KeyExtractorType::PREFIX_BYTES)) {
        const int32_t prefix_len = (extractor).prefix_length;
        if ((rusty::detail::deref_if_pointer_like(prefix_len) <= 0) || (((static_cast<size_t>(prefix_len))) > ((key)).size())) {
            return -1;
        }
        size_t n = static_cast<size_t>(prefix_len);
        if (rusty::detail::deref_if_pointer_like(n) > 8) {
            n = static_cast<size_t>(8);
        }
        int64_t result = static_cast<int64_t>(0);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
            result = ((rusty::detail::deref_if_pointer_like(result) << 8)) | ((static_cast<int64_t>((static_cast<uint8_t>((key)[i])))));
            i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
        }
        return std::move(result);
    }
    if (rusty::detail::deref_if_pointer_like((extractor).kind) == rusty::clone(KeyExtractorType::HASH_MOD)) {
        int64_t hash = static_cast<int64_t>(0);
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < ((key)).size()) {
            hash = rusty::detail::deref_if_pointer_like(hash) ^ ((static_cast<int64_t>((static_cast<uint8_t>((key)[i])))));
            hash = ((rusty::detail::deref_if_pointer_like(hash) << 7)) | ((rusty::detail::deref_if_pointer_like(hash) >> 57));
            i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
        }
        if (rusty::detail::deref_if_pointer_like(hash) < 0) {
            return -hash;
        }
        return std::move(hash);
    }
    return -1;
}
/*RUSTYCPP:GEN-END id=sharding_policy_cache.1*/

// ============================================================================
// Global Sharding Policy Cache (Singleton)
// ============================================================================

// @safe - Returns reference to static instance.
inline ShardingPolicyCache& get_sharding_policy_cache() {
    static ShardingPolicyCache instance = ShardingPolicyCache::new_();
    return instance;
}

}  // namespace janus
