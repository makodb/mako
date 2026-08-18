// mtree_abi.cc - implementation of the pure C ABI over concurrent_btree.
//
// Two rules govern every function here, and both are structural rather
// than advisory:
//
//   1. RCU EPOCHS ARE OPENED AND CLOSED INSIDE ONE FUNCTION. Nothing that
//      escapes depends on an epoch still being held, so callers never see
//      RCU. No epoch is ever held across IO, a blocking call, or caller
//      code -- scoped_rcu_region pins a per-core spinlock that the ticker
//      daemon must take to advance every lagging core, so doing so freezes
//      reclamation process-wide.
//
//   2. NO EXCEPTION CROSSES THE BOUNDARY. `extern "C"` does NOT imply
//      noexcept, and the tree can throw (mbtree::size() builds a
//      std::vector under the epoch, so bad_alloc is reachable). A foreign
//      C++ exception unwinding a caller frame compiled with panic=abort is
//      undefined behaviour, and Rust's catch_unwind does not catch C++
//      exceptions -- so the guard has to live here.

#include "mtree_abi.h"

#include "mako/masstree_btree.h"
#include "mako/varkey.h"
#include "mako/core.h"
#include "mako/silo_runtime.h"

#include <string.h>

#include <string>
#include <vector>

namespace {

// The tree handle is just the btree; the opaque struct exists so the C
// header never names a C++ type.
struct mtx_tree_impl {
  concurrent_btree tree;
};

inline concurrent_btree *as_tree(mtx_tree *t) {
  return &reinterpret_cast<mtx_tree_impl *>(t)->tree;
}

inline varkey to_varkey(const char *k, size_t klen) {
  return varkey(reinterpret_cast<const uint8_t *>(k), klen);
}

// Collector for the range walks. Copies keys into the caller's arena and
// stops as soon as either the entry buffer or the arena is full, so the
// walk itself never allocates and never runs caller code.
struct chunk_collector {
  mtx_kv *out;
  size_t cap;
  char *arena;
  size_t arena_cap;
  size_t n = 0;
  size_t used = 0;

  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    if (n >= cap) return false;
    const size_t klen = static_cast<size_t>(k.length());
    if (used + klen > arena_cap) return false;  // arena full: stop cleanly
    memcpy(arena + used, k.data(), klen);
    out[n].key_off = static_cast<uint32_t>(used);
    out[n].key_len = static_cast<uint32_t>(klen);
    out[n].word = reinterpret_cast<uint64_t>(v);
    used += klen;
    n++;
    return true;
  }
};

// Has this thread been through mtx_thread_attach()? Catching the omission
// is worth a thread_local bool: the failure it prevents is a lazily
// allocated core ID inside a tree call, which aborts the process on
// exhaustion instead of returning a status.
thread_local bool tl_attached = false;

}  // namespace

extern "C" {

uint32_t mtx_abi_version(void) { return MTX_ABI_VERSION; }

size_t mtx_kv_size(void) { return sizeof(mtx_kv); }

int mtx_thread_attach(void) {
  try {
    if (tl_attached) return MTX_OK;
    // masstree's threadinfo hardcodes rcu::s_instance / ticker::s_instance
    // regardless of which SiloRuntime allocated this thread's core ID, so
    // a thread bound elsewhere would index another runtime's per-core RCU
    // slots. Surface that here rather than as a corrupted traversal later.
    if (SiloRuntime::Current() != SiloRuntime::GlobalDefault()) {
      return MTX_ERR_WRONG_RUNTIME;
    }
    if (!SiloRuntime::Current()->try_register_current_thread()) {
      return MTX_ERR_NO_CORE_ID;  // 512-per-runtime space exhausted
    }
    tl_attached = true;
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

mtx_tree *mtx_create(void) {
  try {
    return reinterpret_cast<mtx_tree *>(new mtx_tree_impl());
  } catch (...) {
    return nullptr;
  }
}

void mtx_destroy(mtx_tree *t) {
  if (t == nullptr) return;
  try {
    delete reinterpret_cast<mtx_tree_impl *>(t);
  } catch (...) {
    // A destructor that throws has nowhere to report; swallowing beats
    // unwinding into C.
  }
}

int mtx_get(mtx_tree *t, const char *key, size_t klen, uint64_t *out) {
  if (t == nullptr || key == nullptr || out == nullptr) return MTX_ERR_INVALID;
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    *out = MTX_WORD_NULL;
    const auto region = scoped_rcu_region();
    concurrent_btree::value_type v{};
    if (as_tree(t)->search(to_varkey(key, klen), v)) {
      *out = reinterpret_cast<uint64_t>(v);
    }
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

int mtx_get_or_insert(mtx_tree *t, const char *key, size_t klen, uint64_t word,
                      uint64_t *out) {
  if (t == nullptr || key == nullptr || out == nullptr) return MTX_ERR_INVALID;
  if (word == MTX_WORD_NULL) return MTX_ERR_INVALID;  // reserved
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    const auto region = scoped_rcu_region();
    concurrent_btree *tree = as_tree(t);
    const varkey k = to_varkey(key, klen);

    // Probe first: the overwhelmingly common case is that the key already
    // exists, and a probe is cheaper than a failed insert.
    concurrent_btree::value_type v{};
    if (tree->search(k, v)) {
      *out = reinterpret_cast<uint64_t>(v);
      return MTX_OK;
    }
    if (tree->insert_if_absent(
            k, reinterpret_cast<concurrent_btree::value_type>(word))) {
      *out = word;
      return MTX_OK;
    }
    // Lost the race between the probe and the insert: report the winner.
    if (tree->search(k, v)) {
      *out = reinterpret_cast<uint64_t>(v);
      return MTX_OK;
    }
    // insert_if_absent said "present" and search says "absent". Not
    // reachable while the tree has no remove (which this ABI does not
    // expose, deliberately), so treat it as internal rather than
    // inventing a word.
    return MTX_ERR_INTERNAL;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

int mtx_scan_chunk(mtx_tree *t, const char *from, size_t fromlen, mtx_kv *out,
                   size_t cap, char *arena, size_t arena_cap, size_t *n_out,
                   size_t *arena_used) {
  if (t == nullptr || out == nullptr || n_out == nullptr ||
      arena_used == nullptr || (arena == nullptr && arena_cap != 0)) {
    return MTX_ERR_INVALID;
  }
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    *n_out = 0;
    *arena_used = 0;
    if (cap == 0) return MTX_OK;
    const auto region = scoped_rcu_region();
    chunk_collector c{out, cap, arena, arena_cap};
    varkey lower = to_varkey(from == nullptr ? "" : from,
                             from == nullptr ? 0 : fromlen);
    as_tree(t)->search_range(lower, nullptr, c);
    *n_out = c.n;
    *arena_used = c.used;
    // Nothing fit at all, yet the range is non-empty: the caller's arena
    // is too small for even one key, and silently returning 0 would look
    // like end-of-range and truncate the scan.
    if (c.n == 0 && arena_cap == 0) return MTX_ERR_NO_SPACE;
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

int mtx_rscan_chunk(mtx_tree *t, const char *from, size_t fromlen, mtx_kv *out,
                    size_t cap, char *arena, size_t arena_cap, size_t *n_out,
                    size_t *arena_used) {
  if (t == nullptr || out == nullptr || n_out == nullptr ||
      arena_used == nullptr || (arena == nullptr && arena_cap != 0)) {
    return MTX_ERR_INVALID;
  }
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    *n_out = 0;
    *arena_used = 0;
    if (cap == 0) return MTX_OK;
    const auto region = scoped_rcu_region();
    chunk_collector c{out, cap, arena, arena_cap};
    varkey upper = to_varkey(from == nullptr ? "" : from,
                             from == nullptr ? 0 : fromlen);
    as_tree(t)->rsearch_range(upper, nullptr, c);
    *n_out = c.n;
    *arena_used = c.used;
    if (c.n == 0 && arena_cap == 0) return MTX_ERR_NO_SPACE;
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

int mtx_size(mtx_tree *t, size_t *out) {
  if (t == nullptr || out == nullptr) return MTX_ERR_INVALID;
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    // size() walks raw nodes via tree_walk and asserts in_rcu_region();
    // its own internal guard is compiled out under RcuRespCaller.
    const auto region = scoped_rcu_region();
    *out = as_tree(t)->size();
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

}  // extern "C"
