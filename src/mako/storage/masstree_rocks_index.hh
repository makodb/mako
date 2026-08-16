#pragma once

/**
 * masstree_rocks_index — Masstree as a WRITE-BACK CACHE in front of
 * RocksDB, behind the shared OrderedIndex interface, AUTHORED IN THE
 * RUST DSL (docs/masstree-rocks-cache.md, design + trap list). The
 * #if RUSTYCPP_RUST block is the source of truth; regenerate with
 * scripts/regen_storage_dsl.sh.
 *
 * RocksDB is the system of record; Masstree is a volatile accelerator.
 * A write acks as soon as it is in Masstree — a single flusher thread
 * moves it into RocksDB afterwards — so a crash loses the un-flushed
 * tail. That window is the point of the design, not a defect; callers
 * that need durability call flush().
 *
 * OrderedIndex ONLY — there is no transaction runtime here, the same
 * type fact as masstree_ordered_index: this class implements neither
 * TxnOrderedIndex nor ShardParticipant.
 *
 * Entry lifetime: value and version are IMMUTABLE for the life of an
 * entry. An overwrite allocates a fresh entry and displaces the old
 * one (RCU-deferred free); only the `durable` and `referenced` bytes
 * are ever mutated, and both are atomic. That immutability is what
 * makes the flusher race tractable — the flusher marks durability on
 * the exact instance it read, so a newer write is never mistaken for
 * flushed (trap 2).
 *
 * Per-key striped locks serialize the read-modify-write ops (put /
 * insert / remove / read-through fill). They are needed because this
 * is a two-tier store: "does this key exist?" cannot be answered from
 * Masstree alone, so the check and the publish must not interleave.
 * The critical section can perform a RocksDB read, which is why these
 * are blocking rusty::Mutex stripes and not spinlocks.
 *
 * C++ stays where C++ must: the kernels below do the raw-pointer
 * surgery (RCU arena, tree calls, entry atomics), the rocksdb C API,
 * and the flusher thread. The DSL owns the class shape, the interface
 * attachment, the guard scoping, and the per-op policy.
 *
 * Thread contract: same per-thread bring-up as the rest of the engine
 * (masstree threadinfo + RCU registration — scoped_db_thread_ctx or an
 * mbta-style thread_init covers it). The flusher thread registers
 * itself on entry.
 *
 * INCLUDE ORDER: before any header pulling sto/MassTrans.hh
 * (`#define RCU 1` vs imstring.h's template parameter).
 */

#include "mako/masstree_btree.h"  // concurrent_btree
#include "mako/varkey.h"
#include "abstract_ordered_index.h"

#include <rocksdb/c.h>

#include <stdint.h>
#include <string.h>
#include <atomic>
#include <new>
#include <string>

#include <rusty/condvar.hpp>
#include <rusty/mutex.hpp>
#include <rusty/option.hpp>
#include <rusty/thread.hpp>
#include <rusty/vec.hpp>
#include <rusty/vecdeque.hpp>

// ---------------------------------------------------------------------------
// Cache entry. One RCU allocation; value bytes inline after the header.
// ---------------------------------------------------------------------------

struct mrx_entry {
  uint64_t version;                 // immutable after publish
  uint32_t value_len;               // immutable
  uint8_t tombstone;                // immutable
  std::atomic<uint8_t> durable;     // 0 = dirty, 1 = present in RocksDB
  std::atomic<uint8_t> referenced;  // CLOCK second-chance bit
};

// Probe outcomes, shared by the kernels and the DSL bodies.
#define MRX_MISS 0
#define MRX_LIVE 1
#define MRX_TOMB 2

// @safe - inline payload address
inline uint8_t *mrx_entry_bytes(mrx_entry *e) {
  return reinterpret_cast<uint8_t *>(e) + sizeof(mrx_entry);
}

// @safe - allocation footprint, needed by both dealloc and accounting
inline size_t mrx_entry_footprint(const mrx_entry *e) {
  return sizeof(mrx_entry) + e->value_len;
}

// @unsafe - RCU arena allocation + placement-new of the atomics
inline mrx_entry *mrx_entry_new(uint64_t version, bool tombstone,
                                const std::string &value, bool durable) {
  const size_t payload = tombstone ? 0 : value.size();
  void *p = rcu::s_instance.alloc(sizeof(mrx_entry) + payload);
  mrx_entry *e = new (p) mrx_entry();
  e->version = version;
  e->value_len = static_cast<uint32_t>(payload);
  e->tombstone = tombstone ? 1 : 0;
  e->durable.store(durable ? 1 : 0, std::memory_order_relaxed);
  e->referenced.store(1, std::memory_order_relaxed);
  if (payload != 0) memcpy(mrx_entry_bytes(e), value.data(), payload);
  return e;
}

// @unsafe - deferred free (readers may still hold e); atomics are
// trivially destructible, so skipping the dtor is sound.
inline void mrx_entry_free_rcu(mrx_entry *e) {
  rcu::s_instance.dealloc_rcu(e, mrx_entry_footprint(e));
}

inline varkey mrx_key(lcdf::Str s) {
  return varkey(reinterpret_cast<const uint8_t *>(s.data()), s.length());
}

// ---------------------------------------------------------------------------
// Store: RocksDB handle, dirty queue, striped key locks, flusher thread.
// ---------------------------------------------------------------------------

struct mrx_dirty_item {
  std::string key;
  uint64_t version;  // the entry instance this item refers to
  uint64_t qseq;     // FIFO position, assigned under the queue lock
};

struct mrx_queue_state {
  rusty::VecDeque<mrx_dirty_item> items;
  uint64_t next_qseq{1};
  // Highest qseq made durable. Lives INSIDE the mutex so the flush
  // barrier's condvar predicate can read it without a torn view.
  uint64_t flushed_upto{0};
  bool stopping{false};
};

// A stripe carries no data — it exists purely to serialize
// read-modify-write on the keys that hash to it.
struct mrx_stripe {
  uint8_t pad{0};
};

static const size_t MRX_STRIPES = 1024;

struct mrx_store {
  concurrent_btree *tree{nullptr};

  rocksdb_t *db{nullptr};
  rocksdb_options_t *opts{nullptr};
  rocksdb_readoptions_t *ropts{nullptr};
  rocksdb_writeoptions_t *wopts{nullptr};

  std::atomic<uint64_t> version_ctr{1};
  std::atomic<uint64_t> resident_bytes{0};

  rusty::Mutex<mrx_queue_state> queue{mrx_queue_state()};
  rusty::Condvar queue_cv;    // flusher waits for work
  rusty::Condvar drained_cv;  // flush() waits for the watermark

  rusty::Vec<rusty::Mutex<mrx_stripe>> stripes;

  rusty::Option<rusty::thread::JoinHandle<void>> flusher;
};

// @safe - stripe selection; FNV-1a over the key bytes
inline size_t mrx_stripe_index(lcdf::Str key) {
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < key.length(); i++) {
    h ^= static_cast<uint8_t>(key.data()[i]);
    h *= 1099511628211ULL;
  }
  return static_cast<size_t>(h % MRX_STRIPES);
}

// @unsafe - blocking per-key lock; the guard is a move-only DSL local
inline rusty::MutexGuard<mrx_stripe> mrx_key_lock(mrx_store *s,
                                                  lcdf::Str key) {
  return s->stripes[mrx_stripe_index(key)].lock().unwrap();
}

// RAII region as a move-only value the DSL can hold in a guard local.
// @unsafe - pins the calling thread's RCU epoch
inline scoped_rcu_region mrx_rcu_region() { return scoped_rcu_region(); }

// ---------------------------------------------------------------------------
// Cache-side kernels.
// ---------------------------------------------------------------------------

// @unsafe - reads an RCU-protected entry (caller holds region)
inline mrx_entry *mrx_lookup(concurrent_btree *t, lcdf::Str key) {
  concurrent_btree::value_type v{};
  if (!t->search(mrx_key(key), v)) return nullptr;
  return reinterpret_cast<mrx_entry *>(v);
}

// Probe the cache only. Sets the CLOCK bit on a hit. Returns
// MRX_MISS / MRX_LIVE (value filled) / MRX_TOMB.
// @unsafe - copies out of an RCU-protected buffer (caller holds region)
inline int mrx_cache_probe(concurrent_btree *t, lcdf::Str key,
                           std::string &value, size_t max_bytes_read) {
  mrx_entry *e = mrx_lookup(t, key);
  if (e == nullptr) return MRX_MISS;
  e->referenced.store(1, std::memory_order_relaxed);
  if (e->tombstone) return MRX_TOMB;
  const size_t n = std::min<size_t>(e->value_len, max_bytes_read);
  value.assign(reinterpret_cast<const char *>(mrx_entry_bytes(e)), n);
  return MRX_LIVE;
}

// Publish a new entry instance, displacing any current one, and
// enqueue it for the flusher. Caller MUST hold the key's stripe.
// @unsafe - tree insert + RCU-deferred free of the displaced entry
inline void mrx_publish(mrx_store *s, lcdf::Str key,
                        const std::string &value, bool tombstone) {
  const uint64_t ver = s->version_ctr.fetch_add(1, std::memory_order_relaxed);
  mrx_entry *e = mrx_entry_new(ver, tombstone, value, /*durable=*/false);

  concurrent_btree::value_type old = nullptr;
  s->tree->insert(mrx_key(key), reinterpret_cast<concurrent_btree::value_type>(e),
                  &old);
  s->resident_bytes.fetch_add(mrx_entry_footprint(e), std::memory_order_relaxed);
  if (old != nullptr) {
    mrx_entry *oe = reinterpret_cast<mrx_entry *>(old);
    s->resident_bytes.fetch_sub(mrx_entry_footprint(oe),
                                std::memory_order_relaxed);
    mrx_entry_free_rcu(oe);
  }

  {
    auto g = s->queue.lock().unwrap();
    mrx_dirty_item item;
    item.key.assign(key.data(), key.length());
    item.version = ver;
    item.qseq = (*g).next_qseq++;
    (*g).items.push_back(std::move(item));
  }
  s->queue_cv.notify_one();
}

// Install a CLEAN entry only if the slot is still empty. Used by the
// read-through fill; losing to a concurrent publish is the correct
// outcome (trap 3), so the loser buffer is freed immediately — it was
// never published.
// @unsafe - put-if-absent on the tree
inline bool mrx_fill_clean(mrx_store *s, lcdf::Str key,
                           const std::string &value, bool tombstone) {
  const uint64_t ver = s->version_ctr.fetch_add(1, std::memory_order_relaxed);
  mrx_entry *e = mrx_entry_new(ver, tombstone, value, /*durable=*/true);
  if (s->tree->insert_if_absent(mrx_key(key),
                                reinterpret_cast<concurrent_btree::value_type>(e))) {
    s->resident_bytes.fetch_add(mrx_entry_footprint(e),
                                std::memory_order_relaxed);
    return true;
  }
  rcu::s_instance.dealloc(e, mrx_entry_footprint(e));
  return false;
}

// ---------------------------------------------------------------------------
// RocksDB-side kernels.
// ---------------------------------------------------------------------------

// @unsafe - rocksdb C API; err is leaked to the caller-free contract
inline bool mrx_db_get(mrx_store *s, lcdf::Str key, std::string &value,
                       size_t max_bytes_read) {
  size_t vlen = 0;
  char *err = nullptr;
  char *v = rocksdb_get(s->db, s->ropts, key.data(),
                        static_cast<size_t>(key.length()), &vlen, &err);
  if (err != nullptr) {
    rocksdb_free(err);
    return false;
  }
  if (v == nullptr) return false;
  value.assign(v, std::min<size_t>(vlen, max_bytes_read));
  rocksdb_free(v);
  return true;
}

// @unsafe - rocksdb C API
inline bool mrx_db_exists(mrx_store *s, lcdf::Str key) {
  std::string sink;
  return mrx_db_get(s, key, sink, std::string::npos);
}

// ---------------------------------------------------------------------------
// Two-tier existence + fill, both under the caller's stripe lock.
// ---------------------------------------------------------------------------

// "Does this key exist anywhere?" — cache first (a tombstone is an
// authoritative NO), else the system of record.
// @unsafe - touches both tiers
inline bool mrx_exists(mrx_store *s, lcdf::Str key) {
  mrx_entry *e = mrx_lookup(s->tree, key);
  if (e != nullptr) return e->tombstone == 0;
  return mrx_db_exists(s, key);
}

// Read-through fill. Caller holds the stripe and has already re-probed
// the cache. Installs a negative entry on a RocksDB miss so that a
// later insert() can decide put-if-absent without another disk read.
// @unsafe - touches both tiers
inline bool mrx_fill_from_db(mrx_store *s, lcdf::Str key, std::string &value,
                             size_t max_bytes_read) {
  std::string full;
  if (!mrx_db_get(s, key, full, std::string::npos)) {
    mrx_fill_clean(s, key, std::string(), /*tombstone=*/true);
    return false;
  }
  mrx_fill_clean(s, key, full, /*tombstone=*/false);
  value.assign(full.data(), std::min<size_t>(full.size(), max_bytes_read));
  return true;
}

// ---------------------------------------------------------------------------
// Flusher. ONE thread: FIFO order over the queue is what makes
// `flushed_upto` an exact watermark for the flush() barrier. Splitting
// this into partitioned flushers (as rocksdb_persistence.cc does) means
// giving each partition its own watermark.
// ---------------------------------------------------------------------------

// Drain up to `budget` items into one write batch, commit it, then mark
// the surviving entries durable. The version compare is trap 2: an
// entry displaced by a newer write is a DIFFERENT instance, so this
// never clears dirtiness that a later write earned.
// @unsafe - rocksdb C API + entry atomics
inline size_t mrx_flush_batch(mrx_store *s, size_t budget) {
  rusty::Vec<mrx_dirty_item> taken;
  {
    auto g = s->queue.lock().unwrap();
    while (!(*g).items.is_empty() && taken.len() < budget) {
      taken.push((*g).items.pop_front());
    }
  }
  if (taken.len() == 0) return 0;

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  {
    const auto region = mrx_rcu_region();
    for (size_t i = 0; i < taken.len(); i++) {
      mrx_entry *e = mrx_lookup(s->tree, lcdf::Str(taken[i].key));
      // Superseded (or already evicted): the newer instance carries its
      // own queue item, so dropping this one loses nothing.
      if (e == nullptr || e->version != taken[i].version) continue;
      if (e->tombstone) {
        rocksdb_writebatch_delete(batch, taken[i].key.data(),
                                  taken[i].key.size());
      } else {
        rocksdb_writebatch_put(
            batch, taken[i].key.data(), taken[i].key.size(),
            reinterpret_cast<const char *>(mrx_entry_bytes(e)), e->value_len);
      }
    }
  }

  char *err = nullptr;
  rocksdb_write(s->db, s->wopts, batch, &err);
  rocksdb_writebatch_destroy(batch);
  if (err != nullptr) {
    rocksdb_free(err);
    // Leave the items dirty-but-dropped: the entries stay non-durable,
    // so eviction still refuses them and no data is silently lost.
    return taken.len();
  }

  uint64_t high = 0;
  {
    const auto region = mrx_rcu_region();
    for (size_t i = 0; i < taken.len(); i++) {
      mrx_entry *e = mrx_lookup(s->tree, lcdf::Str(taken[i].key));
      if (e != nullptr && e->version == taken[i].version) {
        e->durable.store(1, std::memory_order_release);
      }
      if (taken[i].qseq > high) high = taken[i].qseq;
    }
  }
  // FIFO pop by a single flusher means `high` only ever grows, so this
  // is a true watermark.
  {
    auto g = s->queue.lock().unwrap();
    if (high > (*g).flushed_upto) (*g).flushed_upto = high;
  }
  s->drained_cv.notify_all();
  return taken.len();
}

// @unsafe - thread body. No explicit bring-up: masstree builds a
// simple_threadinfo per call and rcu::mysync() registers the thread
// lazily on first use, so entering an RCU region is the whole contract.
inline void mrx_flusher_loop(mrx_store *s) {
  for (;;) {
    {
      auto g = s->queue.lock().unwrap();
      g = s->queue_cv
              .wait_while(std::move(g),
                          [](const mrx_queue_state &q) {
                            return q.items.is_empty() && !q.stopping;
                          })
              .unwrap();
      if ((*g).items.is_empty() && (*g).stopping) break;
    }
    while (mrx_flush_batch(s, 256) != 0) {
    }
  }
}

// Block until everything enqueued before this call is durable. The
// target is the last qseq handed out at entry, so writes racing in
// after the call are deliberately not waited on.
// @unsafe - condvar wait against the flusher's watermark
inline void mrx_flush_barrier(mrx_store *s) {
  auto g = s->queue.lock().unwrap();
  const uint64_t target = (*g).next_qseq - 1;
  if (target == 0 || (*g).flushed_upto >= target) return;
  s->queue_cv.notify_one();
  g = s->drained_cv
          .wait_while(std::move(g),
                      [target](const mrx_queue_state &q) {
                        return q.flushed_upto < target && !q.stopping;
                      })
          .unwrap();
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// @unsafe - opens RocksDB and starts the flusher; returns null on failure
inline mrx_store *mrx_store_open(concurrent_btree *tree,
                                 const std::string &db_path) {
  mrx_store *s = new mrx_store();
  s->tree = tree;

  s->opts = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(s->opts, 1);
  s->ropts = rocksdb_readoptions_create();
  s->wopts = rocksdb_writeoptions_create();
  // The whole point: the ack does not wait on fsync.
  rocksdb_writeoptions_set_sync(s->wopts, 0);

  char *err = nullptr;
  s->db = rocksdb_open(s->opts, db_path.c_str(), &err);
  if (err != nullptr) {
    rocksdb_free(err);
    delete s;
    return nullptr;
  }

  for (size_t i = 0; i < MRX_STRIPES; i++) {
    s->stripes.push(rusty::Mutex<mrx_stripe>(mrx_stripe()));
  }

  s->flusher = rusty::Option<rusty::thread::JoinHandle<void>>(
      rusty::thread::spawn([s]() { mrx_flusher_loop(s); }));
  return s;
}

// @unsafe - drains, stops the flusher, closes RocksDB
inline void mrx_store_close(mrx_store *s) {
  if (s == nullptr) return;
  mrx_flush_barrier(s);
  {
    auto g = s->queue.lock().unwrap();
    (*g).stopping = true;
  }
  s->queue_cv.notify_all();
  s->drained_cv.notify_all();
  if (s->flusher.is_some()) {
    s->flusher.take().unwrap().join();
  }
  rocksdb_close(s->db);
  rocksdb_readoptions_destroy(s->ropts);
  rocksdb_writeoptions_destroy(s->wopts);
  rocksdb_options_destroy(s->opts);
  delete s;
}

// @unsafe - estimate; the tree's own count
inline size_t mrx_size(const concurrent_btree *t) { return t->size(); }

// @safe - resident footprint of the cache tier
inline uint64_t mrx_resident_bytes(const mrx_store *s) {
  return s->resident_bytes.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Ranges.
//
// S1 IMPLEMENTATION: drain, then iterate RocksDB alone. That is
// *correct* — after the barrier every acked write is in RocksDB and
// every tombstone has become a RocksDB delete, so the durable tier is
// the whole truth — but it pays a full flush per scan and reads from
// disk what memory already holds. S2 replaces both bodies with the
// chunked two-way merge described in docs/masstree-rocks-cache.md.
// The signatures and boundary semantics are fixed here so that swap is
// body-only.
//
// OPEN (S2): rscan's exact bound inclusivity is matched to masstree's
// rsearch_range at merge time; S1 implements descending (end, start].
// ---------------------------------------------------------------------------

// @safe - lexicographic byte compare, shorter-is-less on a shared prefix
inline int mrx_bytes_cmp(const char *a, size_t alen, const char *b,
                         size_t blen) {
  const size_t n = std::min(alen, blen);
  const int c = (n == 0) ? 0 : memcmp(a, b, n);
  if (c != 0) return c;
  if (alen < blen) return -1;
  if (alen > blen) return 1;
  return 0;
}

// @unsafe - rocksdb iterator; [start, *end) ascending
inline void mrx_scan(mrx_store *s, const std::string &start_key,
                     const std::string *end_key, oi_scan_callback &cb) {
  mrx_flush_barrier(s);
  rocksdb_iterator_t *it = rocksdb_create_iterator(s->db, s->ropts);
  rocksdb_iter_seek(it, start_key.data(), start_key.size());
  std::string val;
  while (rocksdb_iter_valid(it)) {
    size_t klen = 0;
    const char *k = rocksdb_iter_key(it, &klen);
    if (end_key != nullptr &&
        mrx_bytes_cmp(k, klen, end_key->data(), end_key->size()) >= 0) {
      break;
    }
    size_t vlen = 0;
    const char *v = rocksdb_iter_value(it, &vlen);
    val.assign(v, vlen);
    if (!cb.invoke(k, klen, val)) break;
    rocksdb_iter_next(it);
  }
  rocksdb_iter_destroy(it);
}

// @unsafe - rocksdb iterator; descending from start down to *end exclusive
inline void mrx_rscan(mrx_store *s, const std::string &start_key,
                      const std::string *end_key, oi_scan_callback &cb) {
  mrx_flush_barrier(s);
  rocksdb_iterator_t *it = rocksdb_create_iterator(s->db, s->ropts);
  rocksdb_iter_seek_for_prev(it, start_key.data(), start_key.size());
  std::string val;
  while (rocksdb_iter_valid(it)) {
    size_t klen = 0;
    const char *k = rocksdb_iter_key(it, &klen);
    if (end_key != nullptr &&
        mrx_bytes_cmp(k, klen, end_key->data(), end_key->size()) <= 0) {
      break;
    }
    size_t vlen = 0;
    const char *v = rocksdb_iter_value(it, &vlen);
    val.assign(v, vlen);
    if (!cb.invoke(k, klen, val)) break;
    rocksdb_iter_prev(it);
  }
  rocksdb_iter_destroy(it);
}

// NOT THREAD SAFE (mbtree::clear contract); drops cached state only —
// RocksDB is untouched, so this is a cache-drop, not a truncate.
inline oi_stats_map mrx_clear(mrx_store *s) {
  mrx_flush_barrier(s);
  s->tree->clear();
  s->resident_bytes.store(0, std::memory_order_relaxed);
  return oi_stats_map();
}

#if RUSTYCPP_RUST
pub struct masstree_rocks_index {
    name: std::string,
    table_id: i32,
    tree: *mut concurrent_btree,
    store: *mut mrx_store,
}

impl masstree_rocks_index {
    // Block until every write acked before this call is in RocksDB.
    fn flush(&mut self) {
        unsafe { mrx_flush_barrier(self.store) }
    }

    fn resident_bytes(&self) -> u64 {
        unsafe { mrx_resident_bytes(self.store) }
    }
}

#[cpp_inherit]
impl OrderedIndex for masstree_rocks_index {
    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let hit = unsafe { mrx_cache_probe(self.tree, key, value, max_bytes_read) };
        if hit == MRX_LIVE {
            return true;
        }
        if hit == MRX_TOMB {
            return false;
        }
        // Miss. Fill under the stripe so a racing publish/flush/evict
        // cannot leave a stale value cached (trap 3), re-probing first
        // because the winner of that race may have filled it already.
        let _klock = unsafe { mrx_key_lock(self.store, key) };
        let again = unsafe { mrx_cache_probe(self.tree, key, value, max_bytes_read) };
        if again == MRX_LIVE {
            return true;
        }
        if again == MRX_TOMB {
            return false;
        }
        unsafe { mrx_fill_from_db(self.store, key, value, max_bytes_read) }
    }

    // Returns "newly inserted", which in a two-tier store means asking
    // RocksDB on a cache miss — the contract is not answerable from
    // memory alone.
    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let _klock = unsafe { mrx_key_lock(self.store, key) };
        let existed = unsafe { mrx_exists(self.store, key) };
        unsafe { mrx_publish(self.store, key, value, false) };
        !existed
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let _klock = unsafe { mrx_key_lock(self.store, key) };
        if unsafe { mrx_exists(self.store, key) } {
            return false;
        }
        unsafe { mrx_publish(self.store, key, value, false) };
        true
    }

    // A delete is a WRITE of a tombstone, never an erase: erasing would
    // let the next get() fall through to the stale RocksDB row (trap 1).
    fn remove(&mut self, key: lcdf::Str) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let _klock = unsafe { mrx_key_lock(self.store, key) };
        if !unsafe { mrx_exists(self.store, key) } {
            return false;
        }
        unsafe { mrx_publish(self.store, key, "", true) };
        true
    }

    fn scan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { mrx_scan(self.store, start_key, end_key, callback) }
    }

    fn rscan(&mut self, start_key: &std::string, end_key: *const std::string, callback: &mut oi_scan_callback, arena: *mut str_arena) {
        unsafe { mrx_rscan(self.store, start_key, end_key, callback) }
    }

    fn size(&self) -> usize {
        unsafe { mrx_size(self.tree) }
    }

    fn clear(&mut self) -> oi_stats_map {
        unsafe { mrx_clear(self.store) }
    }

    fn get_table_id(&mut self) -> i32 {
        self.table_id
    }

    fn get_is_remote(&mut self) -> bool {
        false
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=masstree_rocks_index.1 version=1 rust_sha256=ad75b7d2f3f47fa4ecf3cb9892a7d67e098ae259e948d84bdfdc3a724f062161*/
struct masstree_rocks_index;

struct masstree_rocks_index : public OrderedIndex {
    std::string name;
    int32_t table_id;
    concurrent_btree* tree;
    mrx_store* store;
    masstree_rocks_index(std::string name_init, int32_t table_id_init, concurrent_btree* tree_init, mrx_store* store_init) : OrderedIndex(), name(std::move(name_init)), table_id(std::move(table_id_init)), tree(std::move(tree_init)), store(std::move(store_init)) {}
    masstree_rocks_index(masstree_rocks_index&& other) noexcept : OrderedIndex(), name(std::move(other.name)), table_id(std::move(other.table_id)), tree(std::move(other.tree)), store(std::move(other.store)) {}


    void flush();
    uint64_t resident_bytes() const;
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


inline void masstree_rocks_index::flush() {
    // @unsafe
    {
        mrx_flush_barrier(this->store);
    }
}

inline uint64_t masstree_rocks_index::resident_bytes() const {
    // @unsafe
    {
        return mrx_resident_bytes(this->store);
    }
}

inline bool masstree_rocks_index::get(lcdf::Str key, std::string& value, size_t max_bytes_read) {
    const auto _guard = mrx_rcu_region();
    const auto hit = mrx_cache_probe(this->tree, std::move(key), value, std::move(max_bytes_read));
    if (rusty::detail::deref_if_pointer_like(hit) == rusty::detail::deref_if_pointer_like(MRX_LIVE)) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(hit) == rusty::detail::deref_if_pointer_like(MRX_TOMB)) {
        return false;
    }
    const auto _klock = mrx_key_lock(this->store, std::move(key));
    const auto again = mrx_cache_probe(this->tree, std::move(key), value, std::move(max_bytes_read));
    if (rusty::detail::deref_if_pointer_like(again) == rusty::detail::deref_if_pointer_like(MRX_LIVE)) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(again) == rusty::detail::deref_if_pointer_like(MRX_TOMB)) {
        return false;
    }
    // @unsafe
    {
        return mrx_fill_from_db(this->store, std::move(key), value, std::move(max_bytes_read));
    }
}

inline bool masstree_rocks_index::put(lcdf::Str key, const std::string& value) {
    const auto _guard = mrx_rcu_region();
    const auto _klock = mrx_key_lock(this->store, std::move(key));
    const auto existed = mrx_exists(this->store, std::move(key));
    // @unsafe
    {
        mrx_publish(this->store, std::move(key), value, false);
    }
    return !existed;
}

inline bool masstree_rocks_index::insert(lcdf::Str key, const std::string& value) {
    const auto _guard = mrx_rcu_region();
    const auto _klock = mrx_key_lock(this->store, std::move(key));
    if (mrx_exists(this->store, std::move(key))) {
        return false;
    }
    // @unsafe
    {
        mrx_publish(this->store, std::move(key), value, false);
    }
    return true;
}

inline bool masstree_rocks_index::remove(lcdf::Str key) {
    const auto _guard = mrx_rcu_region();
    const auto _klock = mrx_key_lock(this->store, std::move(key));
    if (!mrx_exists(this->store, std::move(key))) {
        return false;
    }
    // @unsafe
    {
        mrx_publish(this->store, std::move(key), "", true);
    }
    return true;
}

inline void masstree_rocks_index::scan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        mrx_scan(this->store, start_key, end_key, callback);
    }
}

inline void masstree_rocks_index::rscan(const std::string& start_key, const std::string* end_key, oi_scan_callback& callback, str_arena* arena) {
    // @unsafe
    {
        mrx_rscan(this->store, start_key, end_key, callback);
    }
}

inline size_t masstree_rocks_index::size() const {
    // @unsafe
    {
        return mrx_size(this->tree);
    }
}

inline oi_stats_map masstree_rocks_index::clear() {
    // @unsafe
    {
        return mrx_clear(this->store);
    }
}

inline int32_t masstree_rocks_index::get_table_id() {
    return this->table_id;
}

inline bool masstree_rocks_index::get_is_remote() {
    return false;
}
/*RUSTYCPP:GEN-END id=masstree_rocks_index.1*/
