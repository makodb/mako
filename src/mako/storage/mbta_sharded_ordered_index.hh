#ifndef MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH
#define MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH

#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

#include "abstract_ordered_index.h"
#include "benchmarks/benchmark_config.h"
#include "../lib/common.h"
#include "rocks_interface/status.hh"

// ============================================================================
// mbta_sharded_ordered_index — the "Mako routing" backend, AUTHORED IN
// THE RUST DSL (docs/ordered-index-trait-plan.md, backend phase T2).
// The #if RUSTYCPP_RUST block is the source of truth; the transpiler
// regenerates the GEN block (inline-rust --rewrite). The empty
// #[cpp_inherit] impl attaches the FullOrderedIndex base
// (TxnOrderedIndex + ShardParticipant); the inherent impl's methods
// override the inherited virtuals by signature.
//
// C++ stays only where C++ must: the tiny routing helpers below
// (raw-pointer container access the DSL shouldn't hand-roll), the
// [[noreturn]] abort shim (NDB_UNIMPLEMENTED is a macro), and — after
// the GEN block — Status/build/put_mbta/clear sugar that is not part
// of the interface.
// ============================================================================

using shard_table_vec = std::vector<abstract_ordered_index *>;

// @unsafe - raw-pointer container access for the DSL bodies
inline abstract_ordered_index *oi_shard_at(const shard_table_vec *v,
                                           size_t i) {
  return i < v->size() ? (*v)[i] : nullptr;
}
inline size_t oi_shard_count(const shard_table_vec *v) { return v->size(); }

// FNV-1a over the key bytes, mod the shard count — the per-key router.
// @unsafe - reads raw key bytes
inline size_t oi_hash_shard(lcdf::Str key, size_t nshards) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < static_cast<size_t>(key.length()); i++) {
    h = (h ^ static_cast<uint8_t>(key.data()[i])) * 1099511628211ull;
  }
  return static_cast<size_t>(h % nshards);
}
inline abstract_ordered_index *oi_pick_shard(const shard_table_vec *v,
                                             lcdf::Str key) {
  return (*v)[oi_hash_shard(key, v->size())];
}

// @unsafe - aborts (NDB_UNIMPLEMENTED is a C++ macro the DSL can't spell)
[[noreturn]] inline void oi_unimplemented(const char *what) {
  (void)what;
  ALWAYS_ASSERT(false);
  __builtin_unreachable();
}

#if RUSTYCPP_RUST
pub struct mbta_sharded_ordered_index {
    name: std::string,
    shard_tables: shard_table_vec,
}

#[cpp_inherit]
impl FullOrderedIndex for mbta_sharded_ordered_index {
}

impl mbta_sharded_ordered_index {
    // ---- routing ----------------------------------------------------

    fn shard_for_index(&self, idx: usize) -> *mut abstract_ordered_index {
        unsafe { oi_shard_at(&self.shard_tables, idx) }
    }

    fn check_shard(&self, key: lcdf::Str) -> i32 {
        unsafe { oi_hash_shard(key, oi_shard_count(&self.shard_tables)) as i32 }
    }

    // ---- transactional ops (TxnOrderedIndex) ------------------------

    fn get(&mut self, txn: *mut c_void, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).get(txn, key, value, max_bytes_read) }
    }

    fn put(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string) {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).put(txn, key, value) }
    }

    fn insert(&mut self, txn: *mut c_void, key: lcdf::Str, value: &std::string) {
        // Forward to per-shard insert() (transInsert, not transPut).
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).insert(txn, key, value) }
    }

    fn remove(&mut self, txn: *mut c_void, key: lcdf::Str) {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).remove(txn, key) }
    }

    // Txn'd range reads visit every shard (keys are hash-distributed,
    // so there is no global order across shards — historical behavior
    // kept).
    fn scan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        let n = unsafe { oi_shard_count(&self.shard_tables) };
        let mut i: usize = 0;
        while i < n {
            unsafe { (*oi_shard_at(&self.shard_tables, i)).scan(txn, start_key, end_key, callback, arena) };
            i += 1;
        }
    }

    fn rscan(&mut self, txn: *mut c_void, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        let n = unsafe { oi_shard_count(&self.shard_tables) };
        let mut i: usize = 0;
        while i < n {
            unsafe { (*oi_shard_at(&self.shard_tables, i)).rscan(txn, start_key, end_key, callback, arena) };
            i += 1;
        }
    }

    fn scanRemoteOne(&mut self, txn: *mut c_void, start_key: &std::string, end_key: &std::string, value: &mut std::string) {
        // Range op with no per-key owner; only per-table objects serve
        // the remote txn path.
        unsafe { oi_unimplemented("mbta_sharded_ordered_index: scanRemoteOne") }
    }

    // ---- 2PC participant ops (ShardParticipant) ---------------------

    fn shard_get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).shard_get(key, value, max_bytes_read) }
    }

    fn shard_put(&mut self, key: lcdf::Str, value: &std::string) -> *const c_char {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).shard_put(key, value) }
    }

    fn shard_scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) -> bool {
        // Range 2PC ops have no per-key owner; RPC handlers operate on
        // per-table objects, never this routing wrapper.
        unsafe { oi_unimplemented("mbta_sharded_ordered_index: shard_scan") };
        false
    }

    // ---- non-transactional ops (OrderedIndex) -----------------------

    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).get(key, value, max_bytes_read) }
    }

    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).put(key, value) }
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).insert(key, value) }
    }

    fn remove(&mut self, key: lcdf::Str) -> bool {
        unsafe { (*oi_pick_shard(&self.shard_tables, key)).remove(key) }
    }

    // Non-txn scans keep the single-local-shard limitation (plan D5);
    // shard 0 is the local shard in every current deployment shape.
    fn scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { (*oi_shard_at(&self.shard_tables, 0)).scan(start_key, end_key, callback, arena) }
    }

    fn rscan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { (*oi_shard_at(&self.shard_tables, 0)).rscan(start_key, end_key, callback, arena) }
    }

    // ---- bookkeeping -------------------------------------------------

    fn size(&self) -> usize {
        let n = unsafe { oi_shard_count(&self.shard_tables) };
        let mut total: usize = 0;
        let mut i: usize = 0;
        while i < n {
            total += unsafe { (*oi_shard_at(&self.shard_tables, i)).size() };
            i += 1;
        }
        total
    }

    fn get_table_id(&mut self) -> i32 {
        // The wrapper spans per-key tables with their own ids; report
        // the first shard's id for diagnostics.
        if unsafe { oi_shard_count(&self.shard_tables) } == 0 {
            return -1;
        }
        unsafe { (*oi_shard_at(&self.shard_tables, 0)).get_table_id() }
    }

    fn get_is_remote(&mut self) -> bool {
        // The wrapper itself is local; per-key tables carry their own
        // remoteness.
        false
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=mbta_sharded_ordered_index.1 version=1 rust_sha256=495113940cf8ee25530d5acdf8f64e72a5561acd40706c8a30bac025da8e6025*/
struct mbta_sharded_ordered_index;

struct mbta_sharded_ordered_index : public FullOrderedIndex {
    std::string name;
    shard_table_vec shard_tables;
    mbta_sharded_ordered_index(std::string name_init, shard_table_vec shard_tables_init) : FullOrderedIndex(), name(std::move(name_init)), shard_tables(std::move(shard_tables_init)) {}
    mbta_sharded_ordered_index(mbta_sharded_ordered_index&& other) noexcept : FullOrderedIndex(), name(std::move(other.name)), shard_tables(std::move(other.shard_tables)) {}


    abstract_ordered_index* shard_for_index(size_t idx) const;
    int32_t check_shard(lcdf::Str key) const;
    bool get(c_void* txn, lcdf::Str key, std::string& value, size_t max_bytes_read);
    void put(c_void* txn, lcdf::Str key, const std::string& value);
    void insert(c_void* txn, lcdf::Str key, const std::string& value);
    void remove(c_void* txn, lcdf::Str key);
    void scan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void rscan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void scanRemoteOne(c_void* txn, const std::string& start_key, const std::string& end_key, std::string& value);
    bool shard_get(lcdf::Str key, std::string& value, size_t max_bytes_read);
    const c_char* shard_put(lcdf::Str key, const std::string& value);
    bool shard_scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    bool get(lcdf::Str key, std::string& value, size_t max_bytes_read);
    bool put(lcdf::Str key, const std::string& value);
    bool insert(lcdf::Str key, const std::string& value);
    bool remove(lcdf::Str key);
    void scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    void rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena);
    size_t size() const;
    int32_t get_table_id();
    bool get_is_remote();
};


inline abstract_ordered_index* mbta_sharded_ordered_index::shard_for_index(size_t idx) const {
    // @unsafe
    {
        return oi_shard_at(&this->shard_tables, std::move(idx));
    }
}

inline int32_t mbta_sharded_ordered_index::check_shard(lcdf::Str key) const {
    // @unsafe
    {
        return static_cast<int32_t>(oi_hash_shard(std::move(key), oi_shard_count(&this->shard_tables)));
    }
}

inline bool mbta_sharded_ordered_index::get(c_void* txn, lcdf::Str key, std::string& value, size_t max_bytes_read) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).get(txn, std::move(key), value, std::move(max_bytes_read));
    }
}

inline void mbta_sharded_ordered_index::put(c_void* txn, lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).put(txn, std::move(key), value);
    }
}

inline void mbta_sharded_ordered_index::insert(c_void* txn, lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).insert(txn, std::move(key), value);
    }
}

inline void mbta_sharded_ordered_index::remove(c_void* txn, lcdf::Str key) {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).remove(txn, std::move(key));
    }
}

inline void mbta_sharded_ordered_index::scan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    const auto n = oi_shard_count(&this->shard_tables);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, std::move(i))))).scan(txn, start_key, end_key, callback, arena);
        }
        i += 1;
    }
}

inline void mbta_sharded_ordered_index::rscan(c_void* txn, const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    const auto n = oi_shard_count(&this->shard_tables);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, std::move(i))))).rscan(txn, start_key, end_key, callback, arena);
        }
        i += 1;
    }
}

inline void mbta_sharded_ordered_index::scanRemoteOne(c_void* txn, const std::string& start_key, const std::string& end_key, std::string& value) {
    // @unsafe
    {
        oi_unimplemented("mbta_sharded_ordered_index: scanRemoteOne");
    }
}

inline bool mbta_sharded_ordered_index::shard_get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).shard_get(std::move(key), value, std::move(max_bytes_read));
    }
}

inline const c_char* mbta_sharded_ordered_index::shard_put(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).shard_put(std::move(key), value);
    }
}

inline bool mbta_sharded_ordered_index::shard_scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        oi_unimplemented("mbta_sharded_ordered_index: shard_scan");
    }
    return false;
}

inline bool mbta_sharded_ordered_index::get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).get(std::move(key), value, std::move(max_bytes_read));
    }
}

inline bool mbta_sharded_ordered_index::put(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).put(std::move(key), value);
    }
}

inline bool mbta_sharded_ordered_index::insert(lcdf::Str key, const std::string& value) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).insert(std::move(key), std::move(value));
    }
}

inline bool mbta_sharded_ordered_index::remove(lcdf::Str key) {
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_pick_shard(&this->shard_tables, std::move(key))))).remove(std::move(key));
    }
}

inline void mbta_sharded_ordered_index::scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, 0)))).scan(start_key, end_key, callback, arena);
    }
}

inline void mbta_sharded_ordered_index::rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, 0)))).rscan(start_key, end_key, callback, arena);
    }
}

inline size_t mbta_sharded_ordered_index::size() const {
    const auto n = oi_shard_count(&this->shard_tables);
    size_t total = static_cast<size_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        total += ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, std::move(i))))).size();
        i += 1;
    }
    return std::move(total);
}

inline int32_t mbta_sharded_ordered_index::get_table_id() {
    if (oi_shard_count(&this->shard_tables) == 0) {
        return -1;
    }
    // @unsafe
    {
        return ((rusty::detail::deref_if_pointer_like(oi_shard_at(&this->shard_tables, 0)))).get_table_id();
    }
}

inline bool mbta_sharded_ordered_index::get_is_remote() {
    return false;
}
/*RUSTYCPP:GEN-END id=mbta_sharded_ordered_index.1*/

// ============================================================================
// C++ sugar over the DSL class (not part of the interface):
// RocksDB-style Status helpers (used by rocks_interface/LocalTable),
// the open_fn build factory, put_mbta (mbta-specific compare-and-put;
// defined in storage/mbta_wrapper.hh where mbta_ordered_index is
// complete), and clear (test/teardown affordance).
// ============================================================================

inline mako::Status mbta_sharded_Get(mbta_sharded_ordered_index *t,
                                     void *txn, const std::string &key,
                                     std::string &value) {
  bool found = t->get(txn, lcdf::Str(key.data(), key.size()), value,
                      std::string::npos);
  return found ? mako::Status::OK() : mako::Status::NotFound();
}

// NOTE: the value must already be mako::Encode()'d by the caller and
// outlive the commit (the txn'd path stores a pointer, no copy).
inline mako::Status mbta_sharded_Put(mbta_sharded_ordered_index *t,
                                     void *txn, const std::string &key,
                                     const std::string &value) {
  t->put(txn, lcdf::Str(key.data(), key.size()), value);
  return mako::Status::OK();
}

inline mako::Status mbta_sharded_Insert(mbta_sharded_ordered_index *t,
                                        void *txn, const std::string &key,
                                        const std::string &value) {
  // Check existence first: transInsert silently succeeds for
  // duplicates, so dups are detected via Get — the staged read makes
  // insert-if-absent serializable (commit validation catches a racing
  // insert). Gated by rocksdbInterfaceTest I1.4.
  std::string unused;
  if (t->get(txn, lcdf::Str(key.data(), key.size()), unused,
             std::string::npos)) {
    return mako::Status::InvalidArgument("Key already exists");
  }
  t->insert(txn, lcdf::Str(key.data(), key.size()), value);
  return mako::Status::OK();
}

inline mako::Status mbta_sharded_Delete(mbta_sharded_ordered_index *t,
                                        void *txn, const std::string &key) {
  t->remove(txn, lcdf::Str(key.data(), key.size()));
  return mako::Status::OK();
}

// put_mbta is mbta-specific; per-key tables are mbta by construction.
// Defined in storage/mbta_wrapper.hh (needs the complete mbta type).
const char *mbta_sharded_put_mbta(
    mbta_sharded_ordered_index *t, void *txn, lcdf::Str key,
    bool (*compar)(const std::string &newValue, const std::string &oldValue),
    const std::string &value);

// Find-or-create factory over a per-shard opener (historical build()).
inline mbta_sharded_ordered_index *mbta_sharded_build(
    const std::string &name, size_t shard_count,
    const std::function<abstract_ordered_index *(size_t)> &open_fn) {
  ALWAYS_ASSERT(shard_count > 0);
  shard_table_vec tables;
  tables.reserve(shard_count);
  for (size_t i = 0; i < shard_count; i++) {
    tables.push_back(open_fn(i));
  }
  return new mbta_sharded_ordered_index(std::string(name),
                                        std::move(tables));
}

// Aggregate clear across shards (NOT thread safe; teardown affordance).
inline std::map<std::string, uint64_t> mbta_sharded_clear(
    mbta_sharded_ordered_index *t) {
  std::map<std::string, uint64_t> aggregated;
  for (size_t i = 0;; i++) {
    abstract_ordered_index *s = t->shard_for_index(i);
    if (s == nullptr) break;
    for (auto &kv : s->clear()) aggregated[kv.first] += kv.second;
  }
  return aggregated;
}

#endif  // MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH
