#pragma once

/**
 * masstree_ordered_index — plain Masstree (L1) behind the shared
 * OrderedIndex interface, AUTHORED IN THE RUST DSL
 * (docs/storage-interface.md, backend phase T3). The
 * #if RUSTYCPP_RUST block is the source of truth; regenerate with
 * scripts/regen_storage_dsl.sh.
 *
 * OrderedIndex ONLY — masstree has no transaction runtime, and that
 * is a type fact: this class does not implement TxnOrderedIndex or
 * ShardParticipant.
 *
 * Value ownership: masstree stores raw pointers, so this class owns
 * the value allocations. Buffers live in the RCU arena as
 * [u32 len][bytes]; overwrite/remove frees are RCU-deferred and every
 * op pins a scoped_rcu_region (the DSL holds it as a move-only guard
 * local from the oi_rcu_region() factory), so concurrent readers are
 * safe.
 *
 * C++ stays where C++ must: the per-op kernels below do the
 * raw-pointer surgery (RCU-arena alloc/copy/deferred-free, tree calls,
 * and the templated search_range functor bridge, none of which the
 * DSL should hand-roll); the DSL owns the class shape, the interface
 * attachment, the RCU guard scoping, and the bookkeeping logic.
 *
 * Thread contract: same per-thread bring-up as the rest of the engine
 * (masstree threadinfo + RCU registration — scoped_db_thread_ctx or an
 * mbta-style thread_init covers it).
 *
 * INCLUDE ORDER: before any header pulling sto/MassTrans.hh
 * (`#define RCU 1` vs imstring.h's template parameter).
 */

#include "mako/masstree_btree.h"  // concurrent_btree
#include "mako/varkey.h"
#include "abstract_ordered_index.h"

#include <stdint.h>
#include <string.h>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// C++ kernels for the DSL bodies.
// ---------------------------------------------------------------------------

// RAII region as a move-only value the DSL can hold in a guard local.
// @unsafe - pins the calling thread's RCU epoch
inline scoped_rcu_region oi_rcu_region() { return scoped_rcu_region(); }

// @unsafe - RCU arena allocation: [u32 len][bytes]
inline concurrent_btree::value_type oi_mt_make_val(const std::string &value) {
  uint32_t len = static_cast<uint32_t>(value.size());
  auto *p =
      static_cast<uint8_t *>(rcu::s_instance.alloc(sizeof(len) + value.size()));
  memcpy(p, &len, sizeof(len));
  memcpy(p + sizeof(len), value.data(), value.size());
  return p;
}

// @unsafe - deferred free (readers may still hold v)
inline void oi_mt_free_val_rcu(concurrent_btree::value_type v) {
  uint32_t len;
  memcpy(&len, v, sizeof(len));
  rcu::s_instance.dealloc_rcu(v, sizeof(len) + len);
}

inline varkey oi_mt_key(lcdf::Str s) {
  return varkey(reinterpret_cast<const uint8_t *>(s.data()), s.length());
}

// @unsafe - copies out of an RCU-protected buffer (caller holds region)
inline bool oi_mt_get(concurrent_btree *t, lcdf::Str key, std::string &value,
                      size_t max_bytes_read) {
  concurrent_btree::value_type v{};
  if (!t->search(oi_mt_key(key), v)) return false;
  uint32_t len;
  memcpy(&len, v, sizeof(len));
  size_t n = std::min<size_t>(len, max_bytes_read);
  value.assign(reinterpret_cast<const char *>(v) + sizeof(len), n);
  return true;
}

// @unsafe - overwrite; RCU-defers the displaced value (caller holds region)
inline bool oi_mt_put(concurrent_btree *t, lcdf::Str key,
                      const std::string &value) {
  concurrent_btree::value_type old = nullptr;
  bool inserted = t->insert_with_old(oi_mt_key(key), oi_mt_make_val(value), old);
  if (!inserted && old != nullptr) oi_mt_free_val_rcu(old);
  return inserted;
}

// @unsafe - put-if-absent; loser buffer freed immediately (never published)
inline bool oi_mt_insert(concurrent_btree *t, lcdf::Str key,
                         const std::string &value) {
  concurrent_btree::value_type v = oi_mt_make_val(value);
  if (t->insert_if_absent(oi_mt_key(key), v)) return true;
  rcu::s_instance.dealloc(v, sizeof(uint32_t) + value.size());
  return false;
}

// @unsafe - RCU-defers the removed value (caller holds region)
inline bool oi_mt_remove(concurrent_btree *t, lcdf::Str key) {
  concurrent_btree::value_type old = nullptr;
  if (!t->remove_with_old(oi_mt_key(key), old)) return false;
  if (old != nullptr) oi_mt_free_val_rcu(old);
  return true;
}

// Bridges mbtree's templated functor protocol to the shared callback.
// @unsafe - decodes RCU-protected buffers (caller holds region)
struct oi_mt_collector {
  explicit oi_mt_collector(oi_scan_callback &cb) : cb_(cb) {}
  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    uint32_t len;
    memcpy(&len, v, sizeof(len));
    buf_.assign(reinterpret_cast<const char *>(v) + sizeof(len), len);
    return cb_.invoke(k.data(), k.length(), buf_);
  }
  oi_scan_callback &cb_;
  std::string buf_;
};

// @unsafe - [start, *end) ascending (caller holds region)
inline void oi_mt_scan(concurrent_btree *t, const std::string &start_key,
                       const std::string *end_key, oi_scan_callback &cb) {
  oi_mt_collector c(cb);
  varkey lower = oi_mt_key(lcdf::Str(start_key));
  if (end_key != nullptr) {
    varkey upper = oi_mt_key(lcdf::Str(*end_key));
    t->search_range_bounded(lower, upper, c);
  } else {
    t->search_range_unbounded(lower, c);
  }
}

// @unsafe - descending mirror (caller holds region)
inline void oi_mt_rscan(concurrent_btree *t, const std::string &start_key,
                        const std::string *end_key, oi_scan_callback &cb) {
  oi_mt_collector c(cb);
  varkey upper = oi_mt_key(lcdf::Str(start_key));
  if (end_key != nullptr) {
    varkey lower = oi_mt_key(lcdf::Str(*end_key));
    t->rsearch_range_bounded(upper, lower, c);
  } else {
    t->rsearch_range_unbounded(upper, c);
  }
}

inline size_t oi_mt_size(const concurrent_btree *t) { return t->size(); }

// NOT THREAD SAFE (mbtree::clear contract); owned values leak
// deliberately — teardown affordance, not a hot path.
inline oi_stats_map oi_mt_clear(concurrent_btree *t) {
  t->clear();
  return oi_stats_map();
}

#if RUSTYCPP_RUST
pub struct masstree_ordered_index {
    name: std::string,
    table_id: i32,
    tree: *mut concurrent_btree,
}

#[cpp_inherit]
impl OrderedIndex for masstree_ordered_index {
    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_get(self.tree, key, value, max_bytes_read) }
    }

    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_put(self.tree, key, value) }
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_insert(self.tree, key, value) }
    }

    fn remove(&mut self, key: lcdf::Str) -> bool {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_remove(self.tree, key) }
    }

    fn scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_scan(self.tree, start_key, end_key, callback) }
    }

    fn rscan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        let _guard = unsafe { oi_rcu_region() };
        unsafe { oi_mt_rscan(self.tree, start_key, end_key, callback) }
    }

    fn size(&self) -> usize {
        unsafe { oi_mt_size(self.tree) }
    }

    fn clear(&mut self) -> oi_stats_map {
        unsafe { oi_mt_clear(self.tree) }
    }

    fn get_table_id(&mut self) -> i32 {
        self.table_id
    }

    fn get_is_remote(&mut self) -> bool {
        false
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=masstree_ordered_index.1 version=1 rust_sha256=52057791992e6c49a23cec4616869f1ff0ad9b2d2f5ed8b2228471e0dfeeb942*/
struct masstree_ordered_index;

struct masstree_ordered_index : public OrderedIndex {
    std::string name;
    int32_t table_id;
    concurrent_btree* tree;
    masstree_ordered_index(std::string name_init, int32_t table_id_init, concurrent_btree* tree_init) : OrderedIndex(), name(std::move(name_init)), table_id(std::move(table_id_init)), tree(std::move(tree_init)) {}
    masstree_ordered_index(masstree_ordered_index&& other) noexcept : OrderedIndex(), name(std::move(other.name)), table_id(std::move(other.table_id)), tree(std::move(other.tree)) {}


    bool get(lcdf::Str key, std::string& value, size_t max_bytes_read);
    bool put(lcdf::Str key, const std::string& value);
    bool insert(lcdf::Str key, const std::string& value);
    bool remove(lcdf::Str key);
    void scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    size_t size() const;
    oi_stats_map clear();
    int32_t get_table_id();
    bool get_is_remote();
};


inline bool masstree_ordered_index::get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        return oi_mt_get(this->tree, std::move(key), value, std::move(max_bytes_read));
    }
}

inline bool masstree_ordered_index::put(lcdf::Str key, const std::string& value) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        return oi_mt_put(this->tree, std::move(key), value);
    }
}

inline bool masstree_ordered_index::insert(lcdf::Str key, const std::string& value) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        return oi_mt_insert(this->tree, std::move(key), value);
    }
}

inline bool masstree_ordered_index::remove(lcdf::Str key) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        return oi_mt_remove(this->tree, std::move(key));
    }
}

inline void masstree_ordered_index::scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        oi_mt_scan(this->tree, start_key, end_key, callback);
    }
}

inline void masstree_ordered_index::rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        oi_mt_rscan(this->tree, start_key, end_key, callback);
    }
}

inline size_t masstree_ordered_index::size() const {
    // @unsafe
    {
        return oi_mt_size(this->tree);
    }
}

inline oi_stats_map masstree_ordered_index::clear() {
    // @unsafe
    {
        return oi_mt_clear(this->tree);
    }
}

inline int32_t masstree_ordered_index::get_table_id() {
    return this->table_id;
}

inline bool masstree_ordered_index::get_is_remote() {
    return false;
}
/*RUSTYCPP:GEN-END id=masstree_ordered_index.1*/
