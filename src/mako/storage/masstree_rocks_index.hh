#pragma once

/**
 * masstree_rocks_index — Masstree as a WRITE-BACK CACHE in front of
 * RocksDB, behind the shared OrderedIndex interface, AUTHORED IN THE
 * RUST DSL (docs/masstree-rocks-cache.md, design + trap list). The
 * #if RUSTYCPP_RUST block is the source of truth; regenerate with
 * scripts/regen_storage_dsl.sh.
 *
 * Two properties define this file.
 *
 * A WRITE ACKS BEFORE IT IS DURABLE. put/insert/remove land in Masstree
 * and return; a single flusher thread moves them into RocksDB
 * afterwards, so a crash loses the un-flushed tail. Callers that need
 * durability call flush().
 *
 * MASSTREE HOLDS EVERY KEY; ONLY VALUES ARE EVICTABLE. This is the
 * load-bearing invariant: the tree is a complete index of the keyspace,
 * so a TREE MISS IS AUTHORITATIVE ABSENCE. Everything else follows —
 * existence is answerable in memory (so the write ops are lock-free CAS
 * loops rather than two-tier read-modify-writes under per-key locks),
 * eviction swaps a pointer instead of removing a key, and range scans
 * walk one tier instead of merging two.
 *
 * The invariant must hold before the first operation, which is why
 * mrx_store_open scans the whole database to install its keys. Opening
 * a large existing RocksDB is O(keyspace); opening an empty one is
 * free.
 *
 * OrderedIndex ONLY — there is no transaction runtime here, the same
 * type fact as masstree_ordered_index: this class implements neither
 * TxnOrderedIndex nor ShardParticipant.
 *
 * C++ stays where C++ must: the kernels below do the raw-pointer
 * surgery (RCU arena, tree calls, the CAS loops, entry atomics), the
 * rocksdb C API, and the flusher thread. The DSL owns the class shape,
 * the interface attachment, the RCU guard scoping, and the per-op
 * policy.
 *
 * Thread contract: masstree builds a simple_threadinfo per call and
 * rcu::mysync() registers the calling thread lazily, so entering an RCU
 * region is the whole per-thread contract.
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
// Value record. IMMUTABLE once published, except `durable` — durability
// is a property of one specific version, so it is the only mutable
// field and it is atomic. Value bytes follow the header inline.
//
// An EVICTED value is not a null pointer: it is a real record with
// resident = 0 that still carries its version. That is what makes the
// fill CAS safe against ABA (trap 3).
// ---------------------------------------------------------------------------

struct mrx_val {
  uint64_t version;
  uint32_t len;
  uint8_t tombstone;             // the key is deleted
  uint8_t resident;              // 0 = bytes live only in RocksDB
  std::atomic<uint8_t> durable;  // 1 = this version is in RocksDB
};

// One per key, allocated once and never moved: a stable CAS target.
// `val` is never null.
struct mrx_entry {
  std::atomic<mrx_val *> val;
  std::atomic<uint8_t> referenced;  // CLOCK second-chance bit
};

// @safe - inline payload address
inline uint8_t *mrx_val_bytes(mrx_val *v) {
  return reinterpret_cast<uint8_t *>(v) + sizeof(mrx_val);
}

// @safe - allocation footprint, for both dealloc and byte accounting
inline size_t mrx_val_footprint(const mrx_val *v) {
  return sizeof(mrx_val) + v->len;
}

// @unsafe - RCU arena allocation + placement-new of the atomic
inline mrx_val *mrx_val_new(uint64_t version, bool tombstone, bool resident,
                            const std::string &value, bool durable) {
  const size_t payload = (resident && !tombstone) ? value.size() : 0;
  void *p = rcu::s_instance.alloc(sizeof(mrx_val) + payload);
  mrx_val *v = new (p) mrx_val();
  v->version = version;
  v->len = static_cast<uint32_t>(payload);
  v->tombstone = tombstone ? 1 : 0;
  v->resident = resident ? 1 : 0;
  v->durable.store(durable ? 1 : 0, std::memory_order_relaxed);
  if (payload != 0) memcpy(mrx_val_bytes(v), value.data(), payload);
  return v;
}

// @unsafe - deferred free (readers may still hold v); the atomic is
// trivially destructible, so skipping the dtor is sound.
inline void mrx_val_free_rcu(mrx_val *v) {
  rcu::s_instance.dealloc_rcu(v, mrx_val_footprint(v));
}

// @unsafe - immediate free of a record that was never published
inline void mrx_val_drop(mrx_val *v) {
  rcu::s_instance.dealloc(v, mrx_val_footprint(v));
}

// @unsafe - RCU arena allocation of a stable entry
inline mrx_entry *mrx_entry_alloc(mrx_val *initial) {
  void *p = rcu::s_instance.alloc(sizeof(mrx_entry));
  mrx_entry *e = new (p) mrx_entry();
  e->val.store(initial, std::memory_order_relaxed);
  e->referenced.store(1, std::memory_order_relaxed);
  return e;
}

// @safe - copy a resident value out
inline void mrx_val_copy_out(mrx_val *v, std::string &value,
                             size_t max_bytes_read) {
  const size_t n = std::min<size_t>(v->len, max_bytes_read);
  value.assign(reinterpret_cast<const char *>(mrx_val_bytes(v)), n);
}

inline varkey mrx_key(lcdf::Str s) {
  return varkey(reinterpret_cast<const uint8_t *>(s.data()), s.length());
}

// ---------------------------------------------------------------------------
// Results handed back to the DSL. Two bools rather than an int code:
// the DSL would otherwise compare against named constants, and the
// transpiler lowers `==` through a rusty::detail helper the pinned
// runtime (bcd32358) does not carry.
// ---------------------------------------------------------------------------

// done = the cache answered outright; found = the answer.
struct mrx_probe_result {
  bool done;
  bool found;
};

// wrote = the write was applied; existed = the key was live beforehand.
struct mrx_write_result {
  bool wrote;
  bool existed;
};

// ---------------------------------------------------------------------------
// Store: RocksDB handle, dirty queue, flusher thread.
// ---------------------------------------------------------------------------

struct mrx_dirty_item {
  std::string key;
  uint64_t version;  // the value instance this item refers to
  uint64_t qseq;     // FIFO position, assigned under the queue lock
};

struct mrx_queue_state {
  rusty::VecDeque<mrx_dirty_item> items;
  uint64_t next_qseq{1};
  // Highest qseq made durable. Lives INSIDE the mutex so the flush
  // barrier's condvar predicate can read it without a torn view.
  uint64_t flushed_upto{0};
  bool stopping{false};
  // A RocksDB write failed. The affected values were left non-durable
  // (so eviction still refuses them and nothing is silently lost), but
  // the watermark can no longer advance past them — without this flag
  // flush() would block forever waiting on a batch that will never
  // land.
  bool write_failed{false};
};

// Sweeper state. The cursor is a KEY, not an iterator, so it stays
// valid across chunks even as the tree changes underneath — that is
// what makes it usable as a clock hand.
struct mrx_sweep_state {
  std::string cursor;
  // Bumped by the flusher after every committed batch. A sweep that
  // reclaimed nothing waits for this to move rather than spinning:
  // the only thing that can make values evictable is the flusher
  // marking them durable.
  uint64_t flush_epoch{0};
  bool stopping{false};
};

struct mrx_store {
  concurrent_btree *tree{nullptr};

  rocksdb_t *db{nullptr};
  rocksdb_options_t *opts{nullptr};
  rocksdb_readoptions_t *ropts{nullptr};
  rocksdb_writeoptions_t *wopts{nullptr};

  std::atomic<uint64_t> version_ctr{1};
  std::atomic<uint64_t> resident_bytes{0};

  // Byte ceiling for the VALUE tier. 0 = unbounded, which is the
  // default and preserves the pre-eviction behavior exactly. Keys and
  // entries are never reclaimed, so this does not bound total memory.
  uint64_t capacity{0};

  rusty::Mutex<mrx_queue_state> queue{mrx_queue_state()};
  rusty::Condvar queue_cv;    // flusher waits for work
  rusty::Condvar drained_cv;  // flush() waits for the watermark

  rusty::Mutex<mrx_sweep_state> sweep{mrx_sweep_state()};
  rusty::Condvar sweep_cv;  // sweeper waits for pressure or flush progress

  rusty::Option<rusty::thread::JoinHandle<void>> flusher;
  rusty::Option<rusty::thread::JoinHandle<void>> sweeper;
};

// @safe - true when the value tier is over its ceiling
inline bool mrx_over_capacity(const mrx_store *s) {
  if (s->capacity == 0) return false;
  return s->resident_bytes.load(std::memory_order_relaxed) > s->capacity;
}

// @unsafe - nudge the sweeper when a write pushes the tier over
inline void mrx_maybe_wake_sweeper(mrx_store *s) {
  if (mrx_over_capacity(s)) s->sweep_cv.notify_one();
}

// RAII region as a move-only value the DSL can hold in a guard local.
// @unsafe - pins the calling thread's RCU epoch
inline scoped_rcu_region mrx_rcu_region() { return scoped_rcu_region(); }

// @safe - resident footprint of the cache tier
inline uint64_t mrx_resident_bytes(const mrx_store *s) {
  return s->resident_bytes.load(std::memory_order_relaxed);
}

// @safe - byte accounting across a published swap
inline void mrx_account_swap(mrx_store *s, const mrx_val *old_v,
                             const mrx_val *new_v) {
  s->resident_bytes.fetch_add(mrx_val_footprint(new_v),
                              std::memory_order_relaxed);
  s->resident_bytes.fetch_sub(mrx_val_footprint(old_v),
                              std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Tree access.
// ---------------------------------------------------------------------------

// @unsafe - reads an RCU-protected entry (caller holds region)
inline mrx_entry *mrx_lookup(concurrent_btree *t, lcdf::Str key) {
  concurrent_btree::value_type v{};
  if (!t->search(mrx_key(key), v)) return nullptr;
  return reinterpret_cast<mrx_entry *>(v);
}

// Get the entry for a key, creating it if this is the first time the
// key has been written. A fresh entry starts as a DURABLE TOMBSTONE:
// the key does not exist yet and there is nothing in RocksDB to write.
// @unsafe - put-if-absent on the tree; loser allocations freed immediately
inline mrx_entry *mrx_entry_for(mrx_store *s, lcdf::Str key) {
  mrx_entry *e = mrx_lookup(s->tree, key);
  if (e != nullptr) return e;

  mrx_val *v0 = mrx_val_new(0, /*tombstone=*/true, /*resident=*/true,
                            std::string(), /*durable=*/true);
  mrx_entry *ne = mrx_entry_alloc(v0);
  if (s->tree->insert_if_absent(
          mrx_key(key), reinterpret_cast<concurrent_btree::value_type>(ne))) {
    s->resident_bytes.fetch_add(sizeof(mrx_entry) + mrx_val_footprint(v0),
                                std::memory_order_relaxed);
    return ne;
  }
  mrx_val_drop(v0);
  rcu::s_instance.dealloc(ne, sizeof(mrx_entry));
  return mrx_lookup(s->tree, key);
}

// ---------------------------------------------------------------------------
// RocksDB-side kernels.
// ---------------------------------------------------------------------------

// @unsafe - rocksdb C API
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

// ---------------------------------------------------------------------------
// Dirty queue.
// ---------------------------------------------------------------------------

// @unsafe - queue lock; qseq is assigned HERE so FIFO order and
// sequence order agree (publication order alone does not — two threads
// can publish and enqueue in opposite orders).
inline void mrx_enqueue(mrx_store *s, lcdf::Str key, uint64_t version) {
  {
    auto g = s->queue.lock().unwrap();
    mrx_dirty_item item;
    item.key.assign(key.data(), key.length());
    item.version = version;
    item.qseq = (*g).next_qseq++;
    (*g).items.push_back(std::move(item));
  }
  s->queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// The write path. ONE CAS loop serves put / insert / remove, because
// all three are the same read-modify-write with different guards:
//
//   require_absent  — insert: abandon if the key is already live
//   require_present — remove: abandon if the key is already absent
//
// No locks: the entry is stable, so `val` is a stable CAS target, and a
// lost race simply re-reads the new state and retries.
// ---------------------------------------------------------------------------

// @unsafe - CAS loop over an RCU-protected value chain
inline mrx_write_result mrx_write(mrx_store *s, lcdf::Str key,
                                  const std::string &value, bool tombstone,
                                  bool require_absent, bool require_present) {
  mrx_write_result r{false, false};
  mrx_entry *e = mrx_entry_for(s, key);
  if (e == nullptr) return r;

  for (;;) {
    mrx_val *cur = e->val.load(std::memory_order_acquire);
    // A key whose value is merely EVICTED still exists — only a
    // tombstone means absent.
    const bool live = (cur->tombstone == 0);
    r.existed = live;
    if (require_absent && live) return r;
    if (require_present && !live) return r;

    const uint64_t ver =
        s->version_ctr.fetch_add(1, std::memory_order_relaxed);
    mrx_val *nv = mrx_val_new(ver, tombstone, /*resident=*/true, value,
                              /*durable=*/false);
    if (e->val.compare_exchange_weak(cur, nv, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      e->referenced.store(1, std::memory_order_relaxed);
      mrx_account_swap(s, cur, nv);
      mrx_val_free_rcu(cur);
      mrx_enqueue(s, key, ver);
      mrx_maybe_wake_sweeper(s);
      r.wrote = true;
      return r;
    }
    // Lost the race. Drop the unpublished record and retry against
    // whatever is current now.
    mrx_val_drop(nv);
  }
}

// ---------------------------------------------------------------------------
// The read path.
// ---------------------------------------------------------------------------

// Answer from memory alone. `done == false` means only that the value
// is not resident — the KEY's existence is never in question here,
// because a tree miss is authoritative absence.
// @unsafe - reads an RCU-protected value (caller holds region)
inline mrx_probe_result mrx_cache_probe(mrx_store *s, lcdf::Str key,
                                        std::string &value,
                                        size_t max_bytes_read) {
  mrx_probe_result r{true, false};
  mrx_entry *e = mrx_lookup(s->tree, key);
  if (e == nullptr) return r;  // absent, authoritatively
  e->referenced.store(1, std::memory_order_relaxed);

  mrx_val *cur = e->val.load(std::memory_order_acquire);
  if (cur->tombstone) return r;  // deleted
  if (!cur->resident) {
    r.done = false;  // needs a fill
    return r;
  }
  mrx_val_copy_out(cur, value, max_bytes_read);
  r.found = true;
  return r;
}

// Fetch an evicted value from the system of record and install it.
//
// TRAP 3: the CAS names the exact record we read. A writer that
// published a new value — even one already flushed and evicted again —
// replaced that record with a different allocation, so the CAS fails
// and we retry against the new state instead of installing something
// stale. This is why an evicted value is a versioned record and not a
// null pointer.
// @unsafe - rocksdb read + CAS install
inline bool mrx_fill(mrx_store *s, lcdf::Str key, std::string &value,
                     size_t max_bytes_read) {
  mrx_entry *e = mrx_lookup(s->tree, key);
  if (e == nullptr) return false;

  for (;;) {
    mrx_val *cur = e->val.load(std::memory_order_acquire);
    if (cur->tombstone) return false;
    if (cur->resident) {
      mrx_val_copy_out(cur, value, max_bytes_read);
      return true;
    }

    std::string full;
    if (!mrx_db_get(s, key, full, std::string::npos)) {
      // The tree says this key exists but RocksDB has no row. Report
      // absent rather than inventing data.
      return false;
    }
    mrx_val *nv = mrx_val_new(cur->version, /*tombstone=*/false,
                              /*resident=*/true, full, /*durable=*/true);
    if (e->val.compare_exchange_strong(cur, nv, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      mrx_account_swap(s, cur, nv);
      mrx_val_free_rcu(cur);
      // A fill GROWS the resident tier just as a write does, so a
      // read-only workload can push it over the ceiling too.
      mrx_maybe_wake_sweeper(s);
      value.assign(full.data(), std::min<size_t>(full.size(), max_bytes_read));
      return true;
    }
    mrx_val_drop(nv);
  }
}

// Swap a durable resident value for an evicted marker carrying the same
// version. TRAP 4: a non-durable value is the only copy, so it is never
// evictable.
// @unsafe - CAS on an RCU-protected value chain
inline bool mrx_evict_value(mrx_store *s, mrx_entry *e) {
  mrx_val *cur = e->val.load(std::memory_order_acquire);
  if (cur->tombstone || !cur->resident) return false;
  if (cur->durable.load(std::memory_order_acquire) == 0) return false;

  mrx_val *marker = mrx_val_new(cur->version, /*tombstone=*/false,
                                /*resident=*/false, std::string(),
                                /*durable=*/true);
  if (e->val.compare_exchange_strong(cur, marker, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
    mrx_account_swap(s, cur, marker);
    mrx_val_free_rcu(cur);
    return true;
  }
  mrx_val_drop(marker);
  return false;
}

// ---------------------------------------------------------------------------
// Flusher. ONE thread: FIFO order over the queue is what makes
// `flushed_upto` an exact watermark for the flush() barrier.
// ---------------------------------------------------------------------------

// Drain up to `budget` items into one write batch, commit, then mark
// the surviving versions durable.
//
// TRAP 2: the version compare. A value displaced by a newer write is a
// different allocation with a different version and its own queue item,
// so this never marks durability that a later write earned.
// @unsafe - rocksdb C API + value atomics
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
      if (e == nullptr) continue;
      mrx_val *cur = e->val.load(std::memory_order_acquire);
      if (cur->version != taken[i].version) continue;  // superseded
      if (cur->tombstone) {
        rocksdb_writebatch_delete(batch, taken[i].key.data(),
                                  taken[i].key.size());
      } else {
        rocksdb_writebatch_put(
            batch, taken[i].key.data(), taken[i].key.size(),
            reinterpret_cast<const char *>(mrx_val_bytes(cur)), cur->len);
      }
    }
  }

  char *err = nullptr;
  rocksdb_write(s->db, s->wopts, batch, &err);
  rocksdb_writebatch_destroy(batch);
  if (err != nullptr) {
    rocksdb_free(err);
    // Values stay non-durable, so eviction still refuses them and no
    // data is silently lost. Wake any waiting flush() so it reports
    // failure instead of hanging on a watermark that cannot advance.
    {
      auto g = s->queue.lock().unwrap();
      (*g).write_failed = true;
    }
    s->drained_cv.notify_all();
    return taken.len();
  }

  uint64_t high = 0;
  {
    const auto region = mrx_rcu_region();
    for (size_t i = 0; i < taken.len(); i++) {
      mrx_entry *e = mrx_lookup(s->tree, lcdf::Str(taken[i].key));
      if (e != nullptr) {
        mrx_val *cur = e->val.load(std::memory_order_acquire);
        if (cur->version == taken[i].version) {
          cur->durable.store(1, std::memory_order_release);
        }
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

  // Values just became durable, so values just became evictable. Wake a
  // sweeper that stalled for exactly this.
  {
    auto g = s->sweep.lock().unwrap();
    (*g).flush_epoch++;
  }
  s->sweep_cv.notify_all();
  return taken.len();
}

// @unsafe - thread body
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
// after the call are deliberately not waited on. Returns false if the
// wait gave up because a RocksDB write failed or the store is shutting
// down — in both cases some acked writes are NOT durable.
// @unsafe - condvar wait against the flusher's watermark
inline bool mrx_flush_barrier(mrx_store *s) {
  auto g = s->queue.lock().unwrap();
  const uint64_t target = (*g).next_qseq - 1;
  if (target == 0 || (*g).flushed_upto >= target) return true;
  s->queue_cv.notify_one();
  g = s->drained_cv
          .wait_while(std::move(g),
                      [target](const mrx_queue_state &q) {
                        return q.flushed_upto < target && !q.stopping &&
                               !q.write_failed;
                      })
          .unwrap();
  return (*g).flushed_upto >= target;
}

// ---------------------------------------------------------------------------
// Ranges. The key set is wholly in Masstree, so this walks ONE tier:
// emit resident values, skip tombstones, fill evicted ones from
// RocksDB.
//
// The walk is CHUNKED rather than emitting from inside the masstree
// range callback, because filling an evicted value performs a RocksDB
// read and that has no business running inside a tree traversal.
//
// Cost note: a cold range does N point lookups where a merged iterator
// would do one range read. Batching runs of adjacent evicted keys is a
// pure optimization and needs no semantic change.
// ---------------------------------------------------------------------------

struct mrx_chunk_item {
  std::string key;
  mrx_val *val;  // RCU-protected; the caller holds the region
};

// Bridges mbtree's templated functor protocol to a bounded buffer.
// @unsafe - reads RCU-protected entries (caller holds region)
struct mrx_chunk_collector {
  mrx_chunk_collector(rusty::Vec<mrx_chunk_item> &out, size_t budget)
      : out_(out), budget_(budget) {}
  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    if (out_.len() >= budget_) return false;  // stop the walk
    mrx_entry *e = reinterpret_cast<mrx_entry *>(v);
    mrx_chunk_item item;
    item.key.assign(k.data(), k.length());
    item.val = e->val.load(std::memory_order_acquire);
    e->referenced.store(1, std::memory_order_relaxed);
    out_.push(std::move(item));
    return true;
  }
  rusty::Vec<mrx_chunk_item> &out_;
  size_t budget_;
};

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

// Emit one collected item, filling from RocksDB if the value is not
// resident. Returns false if the callback asked to stop.
// @unsafe - may perform a rocksdb read
inline bool mrx_emit_item(mrx_store *s, const mrx_chunk_item &item,
                          oi_scan_callback &cb) {
  if (item.val->tombstone) return true;  // deleted: skip, do not emit
  std::string value;
  if (item.val->resident) {
    mrx_val_copy_out(item.val, value, std::string::npos);
  } else if (!mrx_fill(s, lcdf::Str(item.key), value, std::string::npos)) {
    return true;  // vanished under us; skip
  }
  return cb.invoke(item.key.data(), item.key.size(), value);
}

static const size_t MRX_SCAN_CHUNK = 512;

// @unsafe - [start, *end) ascending
inline void mrx_scan(mrx_store *s, const std::string &start_key,
                     const std::string *end_key, oi_scan_callback &cb) {
  std::string cursor = start_key;
  for (;;) {
    rusty::Vec<mrx_chunk_item> chunk;
    {
      const auto region = mrx_rcu_region();
      mrx_chunk_collector c(chunk, MRX_SCAN_CHUNK);
      varkey lower = mrx_key(lcdf::Str(cursor));
      if (end_key != nullptr) {
        varkey upper = mrx_key(lcdf::Str(*end_key));
        s->tree->search_range(lower, &upper, c);
      } else {
        s->tree->search_range(lower, nullptr, c);
      }
      if (chunk.len() == 0) return;
      for (size_t i = 0; i < chunk.len(); i++) {
        if (!mrx_emit_item(s, chunk[i], cb)) return;
      }
      cursor = chunk[chunk.len() - 1].key;
    }
    if (chunk.len() < MRX_SCAN_CHUNK) return;
    // Resume strictly after the last key: appending a 0 byte yields its
    // immediate successor in byte order.
    cursor.push_back('\0');
  }
}

// @unsafe - descending from start down to *end exclusive
inline void mrx_rscan(mrx_store *s, const std::string &start_key,
                      const std::string *end_key, oi_scan_callback &cb) {
  std::string cursor = start_key;
  bool have_prev = false;
  std::string prev;
  for (;;) {
    rusty::Vec<mrx_chunk_item> chunk;
    {
      const auto region = mrx_rcu_region();
      mrx_chunk_collector c(chunk, MRX_SCAN_CHUNK);
      varkey upper = mrx_key(lcdf::Str(cursor));
      if (end_key != nullptr) {
        varkey lower = mrx_key(lcdf::Str(*end_key));
        s->tree->rsearch_range(upper, &lower, c);
      } else {
        s->tree->rsearch_range(upper, nullptr, c);
      }
      if (chunk.len() == 0) return;
      size_t emitted = 0;
      for (size_t i = 0; i < chunk.len(); i++) {
        // The upper bound is inclusive, so the first item of a resumed
        // chunk repeats the previous chunk's last key.
        if (have_prev && i == 0 &&
            mrx_bytes_cmp(chunk[i].key.data(), chunk[i].key.size(),
                          prev.data(), prev.size()) == 0) {
          continue;
        }
        if (!mrx_emit_item(s, chunk[i], cb)) return;
        emitted++;
      }
      if (emitted == 0) return;
      cursor = chunk[chunk.len() - 1].key;
      prev = cursor;
      have_prev = true;
    }
    if (chunk.len() < MRX_SCAN_CHUNK) return;
  }
}

// ---------------------------------------------------------------------------
// Sweeper: CLOCK reclamation of value bytes.
//
// A rotating cursor walks the keyspace in bounded chunks. A value whose
// reference bit is set gets a second chance — the bit is cleared and
// the value survives this pass — which is what keeps a continuously
// read key resident. Anything else that is durable is evicted.
//
// The cursor is a key rather than an iterator, so it survives
// concurrent tree mutation; running off the end wraps it to the start,
// making it a clock hand rather than a one-shot scan.
// ---------------------------------------------------------------------------

struct mrx_sweep_item {
  std::string key;
  mrx_entry *entry;
};

// @unsafe - reads RCU-protected entries (caller holds region)
struct mrx_sweep_collector {
  mrx_sweep_collector(rusty::Vec<mrx_sweep_item> &out, size_t budget)
      : out_(out), budget_(budget) {}
  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    if (out_.len() >= budget_) return false;
    mrx_sweep_item item;
    item.key.assign(k.data(), k.length());
    item.entry = reinterpret_cast<mrx_entry *>(v);
    out_.push(std::move(item));
    return true;
  }
  rusty::Vec<mrx_sweep_item> &out_;
  size_t budget_;
};

static const size_t MRX_SWEEP_CHUNK = 256;

// One bounded CLOCK pass. Returns bytes reclaimed.
// @unsafe - tree walk + eviction CAS
inline uint64_t mrx_sweep_chunk(mrx_store *s, size_t budget) {
  std::string cursor;
  {
    auto g = s->sweep.lock().unwrap();
    cursor = (*g).cursor;
  }

  const uint64_t before = s->resident_bytes.load(std::memory_order_relaxed);
  bool wrapped = false;
  std::string last;
  {
    const auto region = mrx_rcu_region();
    rusty::Vec<mrx_sweep_item> chunk;
    mrx_sweep_collector c(chunk, budget);
    varkey lower = mrx_key(lcdf::Str(cursor));
    s->tree->search_range(lower, nullptr, c);

    if (chunk.len() == 0) {
      wrapped = true;  // cursor ran past the last key
    } else {
      for (size_t i = 0; i < chunk.len(); i++) {
        mrx_entry *e = chunk[i].entry;
        // Second chance: a recently touched value survives, but pays
        // for it by losing the bit.
        if (e->referenced.exchange(0, std::memory_order_acq_rel) != 0) {
          continue;
        }
        mrx_evict_value(s, e);
        if (!mrx_over_capacity(s)) break;
      }
      last = chunk[chunk.len() - 1].key;
      if (chunk.len() < budget) wrapped = true;
    }
  }

  {
    auto g = s->sweep.lock().unwrap();
    if (wrapped) {
      (*g).cursor.clear();
    } else {
      (*g).cursor = last;
      (*g).cursor.push_back('\0');  // strictly after the last key
    }
  }

  const uint64_t after = s->resident_bytes.load(std::memory_order_relaxed);
  return (before > after) ? (before - after) : 0;
}

// @unsafe - thread body
inline void mrx_sweeper_loop(mrx_store *s) {
  for (;;) {
    {
      auto g = s->sweep.lock().unwrap();
      g = s->sweep_cv
              .wait_while(std::move(g),
                          [s](const mrx_sweep_state &q) {
                            return !q.stopping && !mrx_over_capacity(s);
                          })
              .unwrap();
      if ((*g).stopping) break;
    }

    const uint64_t freed = mrx_sweep_chunk(s, MRX_SWEEP_CHUNK);
    if (freed != 0) continue;

    // Nothing was evictable. Re-sweeping immediately would spin: the
    // only thing that can create an evictable value is the flusher
    // marking one durable, so wait for it to make progress.
    if (!mrx_over_capacity(s)) continue;
    auto g = s->sweep.lock().unwrap();
    const uint64_t seen = (*g).flush_epoch;
    g = s->sweep_cv
            .wait_while(std::move(g),
                        [seen](const mrx_sweep_state &q) {
                          return q.flush_epoch == seen && !q.stopping;
                        })
            .unwrap();
    if ((*g).stopping) break;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Establish the invariant: every key in RocksDB gets a resident entry
// with its value evicted. O(keyspace) on a non-empty database, free on
// an empty one. Nothing may run against the store until this completes.
// @unsafe - rocksdb iterator + tree inserts
inline void mrx_load_keys(mrx_store *s) {
  const auto region = mrx_rcu_region();
  rocksdb_iterator_t *it = rocksdb_create_iterator(s->db, s->ropts);
  rocksdb_iter_seek_to_first(it);
  while (rocksdb_iter_valid(it)) {
    size_t klen = 0;
    const char *k = rocksdb_iter_key(it, &klen);
    mrx_val *v = mrx_val_new(0, /*tombstone=*/false, /*resident=*/false,
                             std::string(), /*durable=*/true);
    mrx_entry *e = mrx_entry_alloc(v);
    if (s->tree->insert_if_absent(
            mrx_key(lcdf::Str(k, static_cast<int>(klen))),
            reinterpret_cast<concurrent_btree::value_type>(e))) {
      s->resident_bytes.fetch_add(sizeof(mrx_entry) + mrx_val_footprint(v),
                                  std::memory_order_relaxed);
    } else {
      mrx_val_drop(v);
      rcu::s_instance.dealloc(e, sizeof(mrx_entry));
    }
    rocksdb_iter_next(it);
  }
  rocksdb_iter_destroy(it);
}

// Open a store. `capacity` bounds the VALUE tier in bytes; 0 (the
// default) disables eviction entirely, which is the pre-eviction
// behavior.
// @unsafe - opens RocksDB, loads the key set, starts the threads
inline mrx_store *mrx_store_open(concurrent_btree *tree,
                                 const std::string &db_path,
                                 uint64_t capacity = 0) {
  mrx_store *s = new mrx_store();
  s->tree = tree;
  s->capacity = capacity;

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

  mrx_load_keys(s);

  s->flusher = rusty::Option<rusty::thread::JoinHandle<void>>(
      rusty::thread::spawn([s]() { mrx_flusher_loop(s); }));
  if (capacity != 0) {
    s->sweeper = rusty::Option<rusty::thread::JoinHandle<void>>(
        rusty::thread::spawn([s]() { mrx_sweeper_loop(s); }));
  }
  return s;
}

// @unsafe - drains, stops the threads, closes RocksDB
inline void mrx_store_close(mrx_store *s) {
  if (s == nullptr) return;
  mrx_flush_barrier(s);

  // Stop the sweeper first: it observes flusher progress, so shutting
  // the flusher first could leave it waiting on an epoch that will
  // never advance.
  {
    auto g = s->sweep.lock().unwrap();
    (*g).stopping = true;
  }
  s->sweep_cv.notify_all();
  if (s->sweeper.is_some()) {
    s->sweeper.take().unwrap().join();
  }

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

// @unsafe - estimate; counts tombstones too, so it OVERCOUNTS live keys
inline size_t mrx_size(const concurrent_btree *t) { return t->size(); }

// TRUNCATE of both tiers, not a cache drop: dropping only the tree
// would break the key-resident invariant while RocksDB still held rows.
// NOT THREAD SAFE (mbtree::clear contract); entry/value allocations are
// leaked deliberately, as in oi_mt_clear — a teardown affordance.
// @unsafe - rocksdb iterator + batch delete + tree clear
inline oi_stats_map mrx_clear(mrx_store *s) {
  mrx_flush_barrier(s);

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(s->db, s->ropts);
  rocksdb_iter_seek_to_first(it);
  while (rocksdb_iter_valid(it)) {
    size_t klen = 0;
    const char *k = rocksdb_iter_key(it, &klen);
    rocksdb_writebatch_delete(batch, k, klen);
    rocksdb_iter_next(it);
  }
  rocksdb_iter_destroy(it);
  char *err = nullptr;
  rocksdb_write(s->db, s->wopts, batch, &err);
  rocksdb_writebatch_destroy(batch);
  if (err != nullptr) rocksdb_free(err);

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
    // False means some of them are not: a RocksDB write failed, or the
    // store is shutting down.
    fn flush(&mut self) -> bool {
        unsafe { mrx_flush_barrier(self.store) }
    }

    fn resident_bytes(&self) -> u64 {
        unsafe { mrx_resident_bytes(self.store) }
    }
}

#[cpp_inherit]
impl OrderedIndex for masstree_rocks_index {
    // A tree miss is authoritative absence, so a miss here never
    // consults RocksDB. Only a NON-RESIDENT VALUE does.
    fn get(&mut self, key: lcdf::Str, value: &mut std::string, max_bytes_read: usize) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let p = unsafe { mrx_cache_probe(self.store, key, value, max_bytes_read) };
        if p.done {
            return p.found;
        }
        unsafe { mrx_fill(self.store, key, value, max_bytes_read) }
    }

    // Returns "newly inserted" — answerable in memory now that every
    // key is resident.
    fn put(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let w = unsafe { mrx_write(self.store, key, value, false, false, false) };
        !w.existed
    }

    fn insert(&mut self, key: lcdf::Str, value: &std::string) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let w = unsafe { mrx_write(self.store, key, value, false, true, false) };
        w.wrote
    }

    // A delete publishes a tombstone version. The key stays in the
    // tree: removing it would break the key-resident invariant, and
    // reclaiming it races with a concurrent insert (trap 1).
    fn remove(&mut self, key: lcdf::Str) -> bool {
        let _guard = unsafe { mrx_rcu_region() };
        let w = unsafe { mrx_write(self.store, key, "", true, false, true) };
        w.wrote
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
/*RUSTYCPP:GEN-BEGIN id=masstree_rocks_index.1 version=1 rust_sha256=1944aabb36725586e1843c335da2ed7c613b80ffa9b9647be7d83c43bfe8d8f8*/
struct masstree_rocks_index;

struct masstree_rocks_index : public OrderedIndex {
    std::string name;
    int32_t table_id;
    concurrent_btree* tree;
    mrx_store* store;
    masstree_rocks_index(std::string name_init, int32_t table_id_init, concurrent_btree* tree_init, mrx_store* store_init) : OrderedIndex(), name(std::move(name_init)), table_id(std::move(table_id_init)), tree(std::move(tree_init)), store(std::move(store_init)) {}
    masstree_rocks_index(masstree_rocks_index&& other) noexcept : OrderedIndex(), name(std::move(other.name)), table_id(std::move(other.table_id)), tree(std::move(other.tree)), store(std::move(other.store)) {}


    bool flush();
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


inline bool masstree_rocks_index::flush() {
    // @unsafe
    {
        return mrx_flush_barrier(this->store);
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
    const auto p = mrx_cache_probe(this->store, std::move(key), value, std::move(max_bytes_read));
    if (p.done) {
        return p.found;
    }
    // @unsafe
    {
        return mrx_fill(this->store, std::move(key), value, std::move(max_bytes_read));
    }
}

inline bool masstree_rocks_index::put(lcdf::Str key, const std::string& value) {
    const auto _guard = mrx_rcu_region();
    const auto w = mrx_write(this->store, std::move(key), value, false, false, false);
    return !w.existed;
}

inline bool masstree_rocks_index::insert(lcdf::Str key, const std::string& value) {
    const auto _guard = mrx_rcu_region();
    const auto w = mrx_write(this->store, std::move(key), value, false, true, false);
    return std::move(w.wrote);
}

inline bool masstree_rocks_index::remove(lcdf::Str key) {
    const auto _guard = mrx_rcu_region();
    const auto w = mrx_write(this->store, std::move(key), "", true, false, true);
    return std::move(w.wrote);
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
