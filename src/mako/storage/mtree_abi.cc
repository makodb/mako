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
#include "mako/sto/thread_registration.hh"

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
  // Non-zero only when the walk stopped for want of arena space; then it
  // is strictly greater than arena_cap. See mtx_scan_chunk.
  size_t needed = 0;

  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    if (n >= cap) return false;
    const size_t klen = static_cast<size_t>(k.length());
    if (used + klen > arena_cap) {
      // The arena filled before the entry buffer did. Record what one
      // more key would have needed, because otherwise this stop is
      // indistinguishable from end-of-range and the caller silently
      // truncates every scan whose keys happen to be long.
      needed = used + klen;
      return false;
    }
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

// An RCU region this thread opened explicitly, via mtx_region_pin.
//
// scoped_rcu_region is RAII and mtx_region_pin/unpin are not, so the
// object is held in raw storage and constructed/destroyed by hand. The
// depth counter makes pinning reentrant; only the outermost pin creates
// or destroys the region.
thread_local unsigned tl_pin_depth = 0;
alignas(scoped_rcu_region) thread_local unsigned char
    tl_pin_storage[sizeof(scoped_rcu_region)];

}  // namespace

extern "C" {

uint32_t mtx_abi_version(void) { return MTX_ABI_VERSION; }

size_t mtx_kv_size(void) { return sizeof(mtx_kv); }

int mtx_thread_attach(void) {
  try {
    if (tl_attached) return MTX_OK;
    if (!mako::silo::claim_thread_runtime(
            mako::silo::thread_runtime::plain_masstree)) {
      return MTX_ERR_WRONG_RUNTIME;
    }
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

int mtx_region_pin(void) {
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    if (tl_pin_depth == 0) {
      new (tl_pin_storage) scoped_rcu_region();
    }
    tl_pin_depth++;
    return MTX_OK;
  } catch (...) {
    return MTX_ERR_INTERNAL;
  }
}

void mtx_region_unpin(void) {
  if (tl_pin_depth == 0) return;  // unbalanced; ignore rather than corrupt
  if (--tl_pin_depth == 0) {
    try {
      reinterpret_cast<scoped_rcu_region *>(tl_pin_storage)
          ->~scoped_rcu_region();
    } catch (...) {
      // A destructor that throws has nowhere to report; swallowing beats
      // unwinding into C.
    }
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

int mtx_insert_if_absent(mtx_tree *t, const char *key, size_t klen,
                         uint64_t word, uint64_t *out) {
  if (t == nullptr || key == nullptr || out == nullptr) return MTX_ERR_INVALID;
  if (word == MTX_WORD_NULL) return MTX_ERR_INVALID;  // reserved
  if (!tl_attached) return MTX_ERR_NOT_ATTACHED;
  try {
    const auto region = scoped_rcu_region();
    concurrent_btree *tree = as_tree(t);
    const varkey k = to_varkey(key, klen);

    // NO leading probe, and that is the entire point of this entry point
    // existing alongside mtx_get_or_insert.
    //
    // The probe in mtx_get_or_insert is justified by "the overwhelmingly
    // common case is that the key already exists". That is true for a
    // caller who knows nothing, and FALSE for the one production caller:
    // the cache's Store::intern only reaches here after ITS OWN lookup
    // has already missed. So the probe re-walked the tree to re-discover
    // a miss the caller had just established, and the insert path paid
    // THREE full traversals where the C++ implementation pays two.
    //
    // Measured with callgrind: reach_leaf self-cost on the insert path
    // was 694.8 instructions per op in Rust -- exactly 3 x 231.6 --
    // against 463.2 (2 x 231.6) in C++, and find_unlocked was exactly
    // 2.00x. That accounted for the whole of the Rust insert path's
    // deficit.
    concurrent_btree::value_type v{};
    if (tree->insert_if_absent(
            k, reinterpret_cast<concurrent_btree::value_type>(word))) {
      *out = word;
      return MTX_OK;
    }
    // Present already -- either from before this call or from a racing
    // writer. Either way the winner is what the caller needs, and this is
    // the same second traversal mtx_get_or_insert does on a lost race.
    if (tree->search(k, v)) {
      *out = reinterpret_cast<uint64_t>(v);
      return MTX_OK;
    }
    // insert_if_absent said "present" and search says "absent". Not
    // reachable while the tree has no remove (which this ABI does not
    // expose, deliberately), so report it rather than invent a word.
    return MTX_ERR_INTERNAL;
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
    as_tree(t)->search_range_unbounded(lower, c);
    *n_out = c.n;
    // The arena-full signal: report what one more key would have needed,
    // which is by construction greater than arena_cap. See the header.
    *arena_used = c.needed != 0 ? c.needed : c.used;
    // Nothing fit at all, yet the range is non-empty: the caller's arena
    // is too small for even one key, and silently returning 0 would look
    // like end-of-range and truncate the scan.
    if (c.n == 0 && (arena_cap == 0 || c.needed != 0)) return MTX_ERR_NO_SPACE;
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
    as_tree(t)->rsearch_range_unbounded(upper, c);
    *n_out = c.n;
    *arena_used = c.needed != 0 ? c.needed : c.used;
    if (c.n == 0 && (arena_cap == 0 || c.needed != 0)) return MTX_ERR_NO_SPACE;
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
