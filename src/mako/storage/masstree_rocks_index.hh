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
 * DIRTY TRACKING is a single MPSC log of {entry, version} tickets with
 * per-thread batching — no global mutex on the write path. A writer
 * CAS-publishes, drops a 16-byte ticket into its own batch (tiny
 * per-writer spinlock, uncontended on the fast path), and returns; full
 * batches are appended to the log with one fetch_add per batch.
 *
 * The flusher DRAINS tickets into an in-memory DIRTY MAP (entry ->
 * oldest undischarged version) at memory speed, recycling ring slots
 * immediately, so producers essentially never see backpressure. A
 * WRITEBACK pass then writes each dirty entry's CURRENTLY PUBLISHED
 * bytes into one RocksDB WriteBatch and erases it on success. The map
 * is both the coalescer (a hot key occupies one map slot no matter how
 * many times it is overwritten — each writeback pass writes it once)
 * and the honesty mechanism: an entry stays in the map until a real
 * write discharges it, and the watermark's min includes the map, so a
 * failed rocksdb_write pins W below the affected versions and the same
 * entries retry next cycle. No obligation can be skipped or lost.
 *
 * DURABILITY is one scalar: persisted_version (W). A published value is
 * durable iff val->version <= W, in the per-key-latest sense: RocksDB
 * holds bytes for that key AT LEAST as new as any version <= W it ever
 * published. W is recomputed as min(version counter, per-writer
 * announce/batch/staged floors, undrained log tickets, dirty-map
 * minimums) - 1. The per-writer `announce` closes the publish gap (a
 * version is drawn by fetch_add BEFORE its CAS publishes, so a
 * preempted writer must still bound W); it is set seq_cst before the
 * draw and cleared only after the ticket is safely in the batch.
 * Eviction tests version <= W — there is no per-value durable flag.
 * Records that never need a write (seed tombstones, open-time key
 * loads, fill results, eviction markers) carry inherited or zero
 * versions already at-or-below W, so they are durable by provenance
 * with no special case.
 *
 * STRAGGLERS: an acked write may sit in its writer's partial batch. The
 * flusher STEALS partial batches every cycle (~100us), so an idle or
 * exited thread cannot pin W or wedge flush(). Writer registration is
 * store-owned and slots are never recycled, so a dead thread's batch
 * remains stealable.
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
 * surgery (RCU arena, tree calls, the CAS loops, entry atomics, the
 * MPSC log), the rocksdb C API, and the background threads. The DSL
 * owns the class shape, the interface attachment, the RCU guard
 * scoping, and the per-op policy.
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

#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <new>
#include <string>
#include <unordered_map>

#include <rusty/condvar.hpp>
#include <rusty/mutex.hpp>
#include <rusty/option.hpp>
#include <rusty/thread.hpp>
#include <rusty/vec.hpp>

// ---------------------------------------------------------------------------
// Value record. IMMUTABLE once published. Value bytes follow the header
// inline. Durability is NOT stored here — it is the comparison
// val->version <= store->persisted_version.
//
// An EVICTED value is not a null pointer: it is a real record with
// resident = 0 that still carries its version. That is what makes the
// fill CAS safe against ABA (trap 3).
// ---------------------------------------------------------------------------

struct mrx_val {
  uint64_t version;
  uint32_t len;
  uint8_t tombstone;  // the key is deleted
  uint8_t resident;   // 0 = bytes live only in RocksDB
};

// @safe - inline payload address
inline uint8_t *mrx_val_bytes(mrx_val *v) {
  return reinterpret_cast<uint8_t *>(v) + sizeof(mrx_val);
}

// @safe - allocation footprint, for both dealloc and byte accounting
inline size_t mrx_val_footprint(const mrx_val *v) {
  return sizeof(mrx_val) + v->len;
}

// @unsafe - RCU arena allocation
inline mrx_val *mrx_val_new(uint64_t version, bool tombstone, bool resident,
                            const std::string &value) {
  const size_t payload = (resident && !tombstone) ? value.size() : 0;
  void *p = rcu::s_instance.alloc(sizeof(mrx_val) + payload);
  mrx_val *v = new (p) mrx_val();
  v->version = version;
  v->len = static_cast<uint32_t>(payload);
  v->tombstone = tombstone ? 1 : 0;
  v->resident = resident ? 1 : 0;
  if (payload != 0) memcpy(mrx_val_bytes(v), value.data(), payload);
  return v;
}

// @unsafe - deferred free (readers may still hold v)
inline void mrx_val_free_rcu(mrx_val *v) {
  rcu::s_instance.dealloc_rcu(v, mrx_val_footprint(v));
}

// @unsafe - immediate free of a record that was never published
inline void mrx_val_drop(mrx_val *v) {
  rcu::s_instance.dealloc(v, mrx_val_footprint(v));
}

// ---------------------------------------------------------------------------
// Per-key entry. Allocated once and never moved or freed: a stable CAS
// target. `val` is never null. The KEY BYTES LIVE INLINE after the
// struct — the flusher needs the key to build RocksDB writes, and
// storing it here (paid once per KEY) is what lets dirty tickets be a
// 16-byte POD instead of carrying a std::string per WRITE.
// ---------------------------------------------------------------------------

struct mrx_entry {
  std::atomic<mrx_val *> val;
  std::atomic<uint8_t> referenced;  // CLOCK second-chance bit
  uint32_t key_len;                 // immutable; bytes follow the struct
};

// @safe - inline key address
inline const char *mrx_entry_key(const mrx_entry *e) {
  return reinterpret_cast<const char *>(e) + sizeof(mrx_entry);
}

// @safe - allocation footprint, for both dealloc and byte accounting
inline size_t mrx_entry_footprint(const mrx_entry *e) {
  return sizeof(mrx_entry) + e->key_len;
}

// @unsafe - RCU arena allocation; copies the key inline
inline mrx_entry *mrx_entry_alloc(mrx_val *initial, lcdf::Str key) {
  void *p = rcu::s_instance.alloc(sizeof(mrx_entry) + key.length());
  mrx_entry *e = new (p) mrx_entry();
  e->val.store(initial, std::memory_order_relaxed);
  e->referenced.store(1, std::memory_order_relaxed);
  e->key_len = static_cast<uint32_t>(key.length());
  memcpy(reinterpret_cast<char *>(e) + sizeof(mrx_entry), key.data(),
         key.length());
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
// The dirty log: one global MPSC ring of tickets, Vyukov-style. A slot
// at position p is FREE when seq == p (mod lap), PUBLISHED when
// seq == p + 1, and is recycled by the flusher to seq = p + CAP after
// its lap is confirmed. Producers reserve a contiguous run with one
// fetch_add per BATCH; a full ring back-pressures the producer (spin +
// usleep), never drops — a dropped ticket is an acked write the flusher
// would never learn about.
// ---------------------------------------------------------------------------

struct mrx_log_item {
  mrx_entry *e;
  uint64_t version;
};

struct mrx_log_slot {
  std::atomic<uint64_t> seq;
  mrx_entry *e;
  uint64_t version;
};

// Ring DEPTH is the coalescing multiplier: a ticket that sits behind a
// deep backlog is usually superseded by drain time and costs nothing at
// RocksDB. A shallow ring degenerates to write-through (measured: 65K
// slots pinned 16 writers to RocksDB's ~270k/s ingest).
static const size_t MRX_LOG_CAP = 1 << 20;   // power of two, ~24MB
static const size_t MRX_BATCH = 64;          // per-writer batch size
static const size_t MRX_DRAIN_BOUND = 65536;    // slots per cycle (memory-speed)
static const size_t MRX_WRITEBACK_CHUNK = 4096; // dirty entries per cycle
static const uint64_t MRX_W_LAZY_CYCLES = 16;

struct mrx_log {
  alignas(64) std::atomic<uint64_t> tail{0};       // reservation
  alignas(64) std::atomic<uint64_t> confirmed{0};  // prefix in RocksDB
  mrx_log_slot slots[MRX_LOG_CAP];
};

// Tiny test-and-test-and-set lock for a writer's batch. Uncontended on
// the owner's fast path (its cache line is core-local); the flusher
// takes it briefly once per steal cycle.
struct mrx_spinlock {
  std::atomic<uint8_t> v{0};
  // @unsafe - spins
  void lock() {
    while (v.exchange(1, std::memory_order_acquire) != 0) {
      while (v.load(std::memory_order_relaxed) != 0) {
      }
    }
  }
  // @safe
  void unlock() { v.store(0, std::memory_order_release); }
};

// Per-writer state. The three floors together cover every version this
// thread has drawn but not yet pushed into the log:
//   announce   — set (seq_cst) BEFORE the version is drawn, cleared
//                after the ticket reaches the batch. Closes the publish
//                gap: a writer preempted between fetch_add and CAS (or
//                between CAS and batching) still bounds W.
//   batch_min  — min version in the local batch (updated under `lock`).
//   staged_min — covers a full batch between leaving `batch` and being
//                published in the log (owner self-append path).
// Coverage hand-off order matters and is release/acquire-chained; see
// the watermark computation in mrx_flusher_cycle.
struct mrx_writer {
  mrx_spinlock lock;
  size_t n{0};
  mrx_log_item batch[MRX_BATCH];
  std::atomic<uint64_t> batch_min{UINT64_MAX};
  std::atomic<uint64_t> staged_min{UINT64_MAX};
  std::atomic<uint64_t> announce{UINT64_MAX};
};

static const size_t MRX_MAX_WRITERS = 256;
static const useconds_t MRX_FLUSH_POLL_US = 100;
static const useconds_t MRX_SWEEP_POLL_US = 500;

// ---------------------------------------------------------------------------
// Store.
// ---------------------------------------------------------------------------

// @safe - process-wide store generation, so a thread_local writer
// cache can never alias a new store that reuses a freed store's address
inline uint64_t mrx_next_store_gen() {
  static std::atomic<uint64_t> g{1};
  return g.fetch_add(1, std::memory_order_relaxed);
}

struct mrx_store {
  const uint64_t gen{mrx_next_store_gen()};

  concurrent_btree *tree{nullptr};

  rocksdb_t *db{nullptr};
  rocksdb_options_t *opts{nullptr};
  rocksdb_readoptions_t *ropts{nullptr};
  rocksdb_writeoptions_t *wopts{nullptr};

  // Hot atomics on their own lines: every write touches version_ctr and
  // resident_bytes, and sharing a line with the cold pointers above
  // caps write throughput on pure coherence traffic.
  alignas(64) std::atomic<uint64_t> version_ctr{1};
  alignas(64) std::atomic<uint64_t> resident_bytes{0};
  // The un-evictable part of resident_bytes: entry records (never
  // freed) plus one mrx_val header per key. Eviction can only reclaim
  // value PAYLOADS, so capacity must be compared against
  // resident_bytes - floor_bytes; comparing against the total made
  // any capacity below the floor permanently "over", degrading the
  // sweeper into a perpetual evict-everything churn (review finding).
  alignas(64) std::atomic<uint64_t> floor_bytes{0};

  // The durability watermark W: every PUBLISHED value with
  // version <= W has its bytes (or its tombstone) in RocksDB. Advanced
  // only by the flusher, monotone.
  alignas(64) std::atomic<uint64_t> persisted_version{0};

  // The most recent rocksdb_write failed; cleared by the next success.
  // flush() consults it so a persistent IO failure reports false
  // instead of blocking forever; the flusher keeps retrying the same
  // log prefix regardless, so a TRANSIENT failure self-heals.
  std::atomic<bool> io_failing{false};
  std::atomic<bool> stopping{false};
  // mrx_clear <-> sweeper handshake: clear sets `clearing`, then waits
  // for `sweep_active` to drop, so no eviction CAS or byte accounting
  // can race the wipe (review finding: resident_bytes.store(0) racing a
  // sweeper fetch_sub underflowed the counter to ~2^64, making
  // over-capacity permanently true).
  std::atomic<bool> clearing{false};
  std::atomic<bool> sweep_active{false};

  // Byte ceiling for the VALUE tier. 0 = unbounded, which is the
  // default and preserves the pre-eviction behavior exactly. Keys and
  // entries are never reclaimed, so this does not bound total memory.
  uint64_t capacity{0};

  mrx_log log;
  std::atomic<uint32_t> n_writers{0};
  mrx_writer writers[MRX_MAX_WRITERS];

  // flush() waiters. The flusher notifies every cycle (~100us), so a
  // classically "lost" wakeup costs one poll interval, never a hang.
  // The counter makes the flusher recompute the watermark EVERY cycle
  // while someone waits (it is lazy otherwise - see the cycle).
  std::atomic<uint32_t> flush_waiters{0};
  rusty::Mutex<uint8_t> flush_mtx{0};
  rusty::Condvar drained_cv;

  uint64_t flusher_cycles{0};  // flusher thread only

  // Stolen tickets that did not fit in the ring yet (flusher-only).
  // At most one batch; included in the watermark min via stash_min.
  mrx_log_item flusher_stash[MRX_BATCH];
  size_t stash_n{0};
  uint64_t stash_min{UINT64_MAX};

  // THE DIRTY MAP (flusher-only): entry -> oldest undischarged ticket
  // version. Drain folds tickets in here at memory speed; writeback
  // discharges entries by writing their CURRENT bytes and erasing.
  // This map is the coalescer (a hot key occupies one slot regardless
  // of ticket count) AND the honesty mechanism (W's min includes it,
  // so the barrier cannot pass an undischarged obligation).
  std::unordered_map<mrx_entry *, uint64_t> dirty;
  // Writeback scratch: entries written in the current batch, erased
  // from `dirty` only after rocksdb_write succeeds.
  rusty::Vec<mrx_entry *> wrote_scratch;

  // CLOCK hand for the sweeper — a KEY, not an iterator, so it survives
  // concurrent tree mutation. Sweeper thread only; no lock needed.
  std::string sweep_cursor;

  rusty::Option<rusty::thread::JoinHandle<void>> flusher;
  rusty::Option<rusty::thread::JoinHandle<void>> sweeper;
};

// @safe - true when the value tier is over its ceiling
inline bool mrx_over_capacity(const mrx_store *s) {
  if (s->capacity == 0) return false;
  const uint64_t total = s->resident_bytes.load(std::memory_order_relaxed);
  const uint64_t floor = s->floor_bytes.load(std::memory_order_relaxed);
  const uint64_t evictable = (total > floor) ? (total - floor) : 0;
  return evictable > s->capacity;
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
// key has been written. A fresh entry starts as a version-0 TOMBSTONE:
// the key does not exist yet and there is nothing in RocksDB to write,
// so version 0 <= W makes it durable by provenance.
// @unsafe - put-if-absent on the tree; loser allocations freed immediately
inline mrx_entry *mrx_entry_for(mrx_store *s, lcdf::Str key) {
  mrx_entry *e = mrx_lookup(s->tree, key);
  if (e != nullptr) return e;

  mrx_val *v0 = mrx_val_new(0, /*tombstone=*/true, /*resident=*/true,
                            std::string());
  mrx_entry *ne = mrx_entry_alloc(v0, key);
  if (s->tree->insert_if_absent(
          mrx_key(key), reinterpret_cast<concurrent_btree::value_type>(ne))) {
    s->resident_bytes.fetch_add(
        mrx_entry_footprint(ne) + mrx_val_footprint(v0),
        std::memory_order_relaxed);
    s->floor_bytes.fetch_add(mrx_entry_footprint(ne) + sizeof(mrx_val),
                             std::memory_order_relaxed);
    return ne;
  }
  mrx_val_drop(v0);
  rcu::s_instance.dealloc(ne, mrx_entry_footprint(ne));
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
// Dirty-log producer side.
// ---------------------------------------------------------------------------

// One writer slot per (thread, store), never recycled — a dead thread's
// partial batch must stay stealable by the flusher.
// @unsafe - thread_local registry
inline mrx_writer *mrx_writer_for(mrx_store *s) {
  struct cache_t {
    mrx_store *store;
    uint64_t gen;
    mrx_writer *w;
  };
  // A small SET of cached slots, not a single entry: with one entry, a
  // thread alternating between two stores (any multi-table workload)
  // would miss on every switch, burn a fresh registry slot each time,
  // and abort() within ~256 operations. Eight entries covers realistic
  // table counts; beyond that, slots burn slowly (review finding).
  static thread_local cache_t c[8] = {};
  static thread_local unsigned victim = 0;
  // Match on pointer AND generation: a new store can reuse a freed
  // store's address, and handing back the stale slot would park tickets
  // where the new store's flusher never looks.
  for (unsigned i = 0; i < 8; i++) {
    if (c[i].store == s && c[i].gen == s->gen) return c[i].w;
  }
  const uint32_t i = s->n_writers.fetch_add(1, std::memory_order_seq_cst);
  if (i >= MRX_MAX_WRITERS) ::abort();  // registry exhausted
  mrx_writer *w = &s->writers[i];
  c[victim].store = s;
  c[victim].gen = s->gen;
  c[victim].w = w;
  victim = (victim + 1) & 7;
  return w;
}

// Append k tickets to the log. Backpressure: a slot still owned by the
// previous lap (flusher behind by a full ring) is WAITED on, never
// skipped — the ticket is the only thing standing between an acked
// write and oblivion.
// @unsafe - Vyukov MPSC producer protocol
inline void mrx_log_append(mrx_store *s, const mrx_log_item *items,
                           size_t k) {
  const uint64_t mask = MRX_LOG_CAP - 1;
  const uint64_t pos = s->log.tail.fetch_add(k, std::memory_order_seq_cst);
  for (size_t i = 0; i < k; i++) {
    mrx_log_slot &sl = s->log.slots[(pos + i) & mask];
    int spins = 0;
    while (sl.seq.load(std::memory_order_acquire) != pos + i) {
      // usleep's real floor is tens of us; yield first so brief
      // backpressure stays cheap.
      if (++spins < 1024) {
        ::sched_yield();
      } else {
        ::usleep(100);
      }
    }
    sl.e = items[i].e;
    sl.version = items[i].version;
    sl.seq.store(pos + i + 1, std::memory_order_release);
  }
}

// Flusher-only: reserve log space WITHOUT ever waiting. Producers may
// fetch_add the tail concurrently, so reserve by CAS with a headroom
// check against `confirmed` (which only the flusher itself advances, so
// it is stable here). The headroom condition t + k <= confirmed + CAP
// guarantees every reserved slot is already recycled, so the publish
// loop never spins. Returns false when the ring lacks room — the
// caller keeps the tickets and retries next cycle. This is what makes
// the flusher immune to the ring backpressure it is itself responsible
// for relieving (review finding: the blocking append could deadlock
// the flusher against its own confirm loop).
// @unsafe - CAS reservation racing producer fetch_adds
inline bool mrx_log_try_append(mrx_store *s, const mrx_log_item *items,
                               size_t k) {
  const uint64_t mask = MRX_LOG_CAP - 1;
  uint64_t t = s->log.tail.load(std::memory_order_relaxed);
  for (int tries = 0; tries < 64; tries++) {
    const uint64_t conf = s->log.confirmed.load(std::memory_order_relaxed);
    if (t + k > conf + MRX_LOG_CAP) return false;  // no room
    if (s->log.tail.compare_exchange_weak(t, t + k,
                                          std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
      for (size_t i = 0; i < k; i++) {
        mrx_log_slot &sl = s->log.slots[(t + i) & mask];
        sl.e = items[i].e;
        sl.version = items[i].version;
        sl.seq.store(t + i + 1, std::memory_order_release);
      }
      return true;
    }
  }
  return false;  // heavy producer contention; retry next cycle
}

// Hand a fresh ticket to the flusher: into the local batch, and into
// the log when the batch fills. Coverage hand-off: batch_min is set
// under the lock BEFORE announce is cleared, and staged_min is set
// BEFORE batch_min is reset, so at every instant some floor <= the
// ticket's version is visible to the watermark computation.
// @unsafe - per-writer lock + log append
inline void mrx_submit(mrx_store *s, mrx_writer *w, mrx_entry *e,
                       uint64_t ver) {
  mrx_log_item staged[MRX_BATCH];
  size_t k = 0;
  w->lock.lock();
  w->batch[w->n].e = e;
  w->batch[w->n].version = ver;
  w->n++;
  if (ver < w->batch_min.load(std::memory_order_relaxed)) {
    w->batch_min.store(ver, std::memory_order_relaxed);
  }
  if (w->n == MRX_BATCH) {
    memcpy(staged, w->batch, sizeof(staged));
    k = w->n;
    w->n = 0;
    w->staged_min.store(w->batch_min.load(std::memory_order_relaxed),
                        std::memory_order_release);
    w->batch_min.store(UINT64_MAX, std::memory_order_release);
  }
  w->lock.unlock();
  w->announce.store(UINT64_MAX, std::memory_order_release);
  if (k != 0) {
    mrx_log_append(s, staged, k);
    w->staged_min.store(UINT64_MAX, std::memory_order_release);
  }
}

// ---------------------------------------------------------------------------
// The write path. ONE CAS loop serves put / insert / remove, because
// all three are the same read-modify-write with different guards:
//
//   require_absent  — insert: abandon if the key is already live
//   require_present — remove: abandon if the key is already absent
//
// No shared locks: the entry is stable, so `val` is a stable CAS
// target; a lost race re-reads the new state and retries; the dirty
// ticket goes into a thread-local batch.
// ---------------------------------------------------------------------------

// @unsafe - CAS loop over an RCU-protected value chain
inline mrx_write_result mrx_write(mrx_store *s, lcdf::Str key,
                                  const std::string &value, bool tombstone,
                                  bool require_absent, bool require_present) {
  mrx_write_result r{false, false};
  mrx_entry *e = mrx_entry_for(s, key);
  if (e == nullptr) return r;

  mrx_writer *w = mrx_writer_for(s);
  // Publish-gap coverage: a floor <= any version this call can draw,
  // made visible (seq_cst, paired with the seq_cst fetch_add below)
  // before the draw. Cleared on every exit path.
  w->announce.store(s->version_ctr.load(std::memory_order_relaxed),
                    std::memory_order_seq_cst);

  for (;;) {
    mrx_val *cur = e->val.load(std::memory_order_acquire);
    // A key whose value is merely EVICTED still exists — only a
    // tombstone means absent.
    const bool live = (cur->tombstone == 0);
    r.existed = live;
    if ((require_absent && live) || (require_present && !live)) {
      w->announce.store(UINT64_MAX, std::memory_order_release);
      return r;
    }

    const uint64_t ver =
        s->version_ctr.fetch_add(1, std::memory_order_seq_cst);
    mrx_val *nv = mrx_val_new(ver, tombstone, /*resident=*/true, value);
    if (e->val.compare_exchange_weak(cur, nv, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      e->referenced.store(1, std::memory_order_relaxed);
      mrx_account_swap(s, cur, nv);
      mrx_val_free_rcu(cur);
      mrx_submit(s, w, e, ver);  // clears announce
      r.wrote = true;
      return r;
    }
    // Lost the race: drop the unpublished record and retry against
    // whatever is current now. The burned version needs no ticket — it
    // was never published, so W owes it nothing — and announce stays
    // set across the retry, so the fresh draw is still covered.
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
//
// The installed record inherits the marker's version, which was <= W
// when the value was evicted, so it is durable by provenance — no
// ticket is issued for a fill.
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
                              /*resident=*/true, full);
    if (e->val.compare_exchange_strong(cur, nv, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      mrx_account_swap(s, cur, nv);
      mrx_val_free_rcu(cur);
      value.assign(full.data(), std::min<size_t>(full.size(), max_bytes_read));
      return true;
    }
    mrx_val_drop(nv);
  }
}

// Swap a covered resident value for an evicted marker carrying the same
// version. TRAP 4, watermark form: a version ABOVE W may not be in
// RocksDB yet, so its bytes are the only copy and it is never
// evictable. (An unconfirmed ticket keeps W below its version, so a
// value with a pending ticket can never pass this guard — which is also
// what makes it impossible for the flusher to meet an eviction marker
// whose version matches a pending ticket.)
// @unsafe - CAS on an RCU-protected value chain
inline bool mrx_evict_value(mrx_store *s, mrx_entry *e) {
  mrx_val *cur = e->val.load(std::memory_order_acquire);
  if (cur->tombstone || !cur->resident) return false;
  if (cur->version > s->persisted_version.load(std::memory_order_acquire)) {
    return false;
  }

  mrx_val *marker = mrx_val_new(cur->version, /*tombstone=*/false,
                                /*resident=*/false, std::string());
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
// Flusher. ONE thread, polling (~100us): drain a published log prefix
// into one WriteBatch, confirm it on success, steal partial writer
// batches, recompute the watermark. Polling instead of condvars — the
// previous wakeup protocol had a verified lost-wakeup, and 10k empty
// cycles/s cost microseconds.
// ---------------------------------------------------------------------------

// @unsafe - the full flusher cycle; returns slots drained + entries written
inline size_t mrx_flusher_cycle(mrx_store *s) {
  const uint64_t mask = MRX_LOG_CAP - 1;

  // --- 1. drain tickets into the dirty map ----------------------------
  // NO RocksDB work here: a ticket is folded into `dirty` (keeping the
  // OLDEST undischarged version per entry) and its slot is confirmed
  // and recycled immediately, so the ring drains at memory speed and
  // producers essentially never see backpressure. Entries are immortal
  // and e->val is not read, so no RCU region is needed.
  const uint64_t base = s->log.confirmed.load(std::memory_order_relaxed);
  uint64_t t0 = s->log.tail.load(std::memory_order_acquire);
  if (t0 - base > MRX_DRAIN_BOUND) t0 = base + MRX_DRAIN_BOUND;
  uint64_t end = base;
  while (end < t0) {
    mrx_log_slot &sl = s->log.slots[end & mask];
    if (sl.seq.load(std::memory_order_acquire) != end + 1) break;  // hole
    auto it = s->dirty.find(sl.e);
    if (it == s->dirty.end()) {
      s->dirty.emplace(sl.e, sl.version);
    } else if (sl.version < it->second) {
      it->second = sl.version;
    }
    sl.seq.store(end + MRX_LOG_CAP, std::memory_order_release);
    end++;
  }
  s->log.confirmed.store(end, std::memory_order_release);
  size_t progressed = static_cast<size_t>(end - base);

  // --- 1b. write back a chunk of the dirty map ------------------------
  // Each selected entry is written ONCE with its CURRENTLY PUBLISHED
  // bytes — at least as new as every undischarged ticket version it
  // covers, which is what makes erasing it honest. (Review finding: the
  // earlier skip-if-superseded drain confirmed obligations without
  // writing anything, so flush() could return true for a hot key that
  // had NEVER reached RocksDB.) A value re-written after our read is
  // re-dirtied by its own ticket on a later drain.
  if (!s->dirty.empty()) {
    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    s->wrote_scratch.clear();
    {
      const auto region = mrx_rcu_region();
      for (auto it = s->dirty.begin();
           it != s->dirty.end() && s->wrote_scratch.len() < MRX_WRITEBACK_CHUNK;
           ++it) {
        mrx_entry *e = it->first;
        mrx_val *cur = e->val.load(std::memory_order_acquire);
        // A non-resident marker is only reachable at version <= W, i.e.
        // its bytes are already in RocksDB; the obligation is met.
        if (cur->resident) {
          if (cur->tombstone) {
            rocksdb_writebatch_delete(batch, mrx_entry_key(e), e->key_len);
          } else {
            rocksdb_writebatch_put(
                batch, mrx_entry_key(e), e->key_len,
                reinterpret_cast<const char *>(mrx_val_bytes(cur)), cur->len);
          }
        }
        s->wrote_scratch.push(e);
      }
    }
    char *err = nullptr;
    rocksdb_write(s->db, s->wopts, batch, &err);
    rocksdb_writebatch_destroy(batch);
    if (err != nullptr) {
      rocksdb_free(err);
      // Obligations stay in the map, W stays pinned below them, and the
      // same entries are retried next cycle: transient IO failure
      // self-heals with no data loss.
      s->io_failing.store(true, std::memory_order_release);
    } else {
      for (size_t i = 0; i < s->wrote_scratch.len(); i++) {
        s->dirty.erase(s->wrote_scratch[i]);
      }
      progressed += s->wrote_scratch.len();
      s->io_failing.store(false, std::memory_order_release);
    }
  }

  // --- 2. steal partial writer batches --------------------------------
  // Bounds the straggler window: an acked write in an idle thread's
  // batch reaches the log within one cycle. All appends here use the
  // non-blocking try-append; tickets that do not fit stay in the
  // flusher's stash (covered by stash_min in the watermark) and retry
  // next cycle, so the flusher can never wedge on ring backpressure.
  if (s->stash_n != 0) {
    if (mrx_log_try_append(s, s->flusher_stash, s->stash_n)) {
      s->stash_n = 0;
      s->stash_min = UINT64_MAX;
    }
  }
  // Clamp: a racing over-limit registration bumps n_writers past the
  // array before its own thread aborts; never index beyond the array.
  uint32_t nw0 = s->n_writers.load(std::memory_order_acquire);
  if (nw0 > MRX_MAX_WRITERS) nw0 = MRX_MAX_WRITERS;
  for (uint32_t i = 0; s->stash_n == 0 && i < nw0; i++) {
    mrx_writer *w = &s->writers[i];
    mrx_log_item buf[MRX_BATCH];
    size_t k = 0;
    uint64_t mn = UINT64_MAX;
    w->lock.lock();
    if (w->n != 0) {
      k = w->n;
      memcpy(buf, w->batch, k * sizeof(mrx_log_item));
      w->n = 0;
      mn = w->batch_min.load(std::memory_order_relaxed);
      w->batch_min.store(UINT64_MAX, std::memory_order_release);
    }
    w->lock.unlock();
    if (k != 0 && !mrx_log_try_append(s, buf, k)) {
      // Ring full: park in the stash. Coverage moves batch_min ->
      // stash_min before anything else runs on this thread, and the
      // watermark below is computed by this same thread, so no version
      // escapes.
      memcpy(s->flusher_stash, buf, k * sizeof(mrx_log_item));
      s->stash_n = k;
      s->stash_min = mn;
    }
  }

  // --- 3. recompute the watermark -------------------------------------
  // W = min(C, per-writer floors, unconfirmed published tickets) - 1.
  // Read order is load-bearing: per-writer floors BEFORE the log scan.
  // A producer clears a floor only AFTER publishing its tickets
  // (release), so if we read the floor as cleared (acquire), the
  // tickets are visible to the scan that follows; if we read it set, it
  // bounds W directly. Either way no version escapes.
  //
  // LAZY under backlog: the scan is O(unconfirmed window), which under
  // deep backpressure is the whole ring. Freshness only matters to
  // flush() waiters (who force it via flush_waiters) and the sweeper
  // (which tolerates a few ms of lag), so a large window is scanned
  // every MRX_W_LAZY_CYCLES cycles instead of every cycle. Skipping
  // never unsounds W - it only lags it.
  s->flusher_cycles++;
  // The dominant cost is iterating the dirty map, so gate on ITS size
  // (the unconfirmed log window is tiny now that drain confirms at
  // memory speed).
  const bool w_due =
      s->dirty.size() <= MRX_WRITEBACK_CHUNK ||
      s->flush_waiters.load(std::memory_order_acquire) != 0 ||
      (s->flusher_cycles % MRX_W_LAZY_CYCLES) == 0;
  if (!w_due) {
    s->drained_cv.notify_all();
    return progressed;
  }
  const uint64_t C = s->version_ctr.load(std::memory_order_seq_cst);
  uint32_t nw = s->n_writers.load(std::memory_order_seq_cst);
  if (nw > MRX_MAX_WRITERS) nw = MRX_MAX_WRITERS;
  uint64_t m = C;
  if (s->stash_n != 0 && s->stash_min < m) m = s->stash_min;
  for (const auto &kv : s->dirty) {
    if (kv.second < m) m = kv.second;
  }
  for (uint32_t i = 0; i < nw; i++) {
    mrx_writer *w = &s->writers[i];
    const uint64_t a = w->announce.load(std::memory_order_acquire);
    if (a < m) m = a;
    const uint64_t b = w->batch_min.load(std::memory_order_acquire);
    if (b < m) m = b;
    const uint64_t st = w->staged_min.load(std::memory_order_acquire);
    if (st < m) m = st;
  }
  {
    const uint64_t conf = s->log.confirmed.load(std::memory_order_relaxed);
    const uint64_t t1 = s->log.tail.load(std::memory_order_acquire);
    for (uint64_t q = conf; q < t1; q++) {
      mrx_log_slot &sl = s->log.slots[q & mask];
      if (sl.seq.load(std::memory_order_acquire) == q + 1) {
        if (sl.version < m) m = sl.version;
      }
      // Unpublished slot: its producer is mid-append and its
      // staged_min/announce — read above, before this scan — still
      // covers those versions.
    }
  }
  const uint64_t w_new = m - 1;  // m >= 1: versions start at 1
  if (w_new > s->persisted_version.load(std::memory_order_relaxed)) {
    s->persisted_version.store(w_new, std::memory_order_release);
  }
  s->drained_cv.notify_all();
  return progressed;
}

// @unsafe - thread body
inline void mrx_flusher_loop(mrx_store *s) {
  while (!s->stopping.load(std::memory_order_acquire)) {
    while (!s->stopping.load(std::memory_order_acquire) &&
           mrx_flusher_cycle(s) != 0) {
    }
    ::usleep(MRX_FLUSH_POLL_US);
  }
  // Final cycle so close() leaves nothing behind and any late barrier
  // waiter is notified.
  mrx_flusher_cycle(s);
}

// Block until everything acked before this call is durable. DURABLE
// MEANS sync=0 RocksDB: in the WAL via the OS page cache — survives a
// process crash, NOT power loss (no fsync anywhere on this path; that
// is the documented level of the whole design). The target
// is the version counter at entry, so writes racing in after the call
// are deliberately not waited on. Returns false if the wait gave up
// because a RocksDB write is failing or the store is shutting down — in
// both cases some acked writes are NOT durable (a transient IO failure
// self-heals: the flusher retries the same log prefix, and io_failing
// clears on the next success).
// @unsafe - condvar wait against the watermark
inline bool mrx_flush_barrier(mrx_store *s) {
  const uint64_t target =
      s->version_ctr.load(std::memory_order_seq_cst) - 1;
  if (target == 0) return true;
  if (s->persisted_version.load(std::memory_order_acquire) >= target) {
    return true;
  }
  s->flush_waiters.fetch_add(1, std::memory_order_acq_rel);
  auto g = s->flush_mtx.lock().unwrap();
  g = s->drained_cv
          .wait_while(std::move(g),
                      [s, target](const uint8_t &) {
                        return s->persisted_version.load(
                                   std::memory_order_acquire) < target &&
                               !s->stopping.load(std::memory_order_acquire) &&
                               !s->io_failing.load(std::memory_order_acquire);
                      })
          .unwrap();
  s->flush_waiters.fetch_sub(1, std::memory_order_acq_rel);
  return s->persisted_version.load(std::memory_order_acquire) >= target;
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

// Materialized under the collector's RCU region so nothing here is
// RCU-protected afterwards: emission — which performs RocksDB point
// reads for non-resident values and runs the caller's arbitrary
// callback — happens with NO region held. Holding one region across a
// whole chunk's emits pinned this core's ticker lock for the duration
// of up to 512 RocksDB reads + callbacks, stalling epoch reclamation
// process-wide (review finding).
struct mrx_chunk_item {
  std::string key;
  std::string value;  // materialized copy when resident
  bool resident;
  bool tombstone;
};

// Bridges mbtree's templated functor protocol to a bounded buffer.
// @unsafe - reads RCU-protected entries (caller holds region)
struct mrx_chunk_collector {
  mrx_chunk_collector(rusty::Vec<mrx_chunk_item> &out, size_t budget)
      : out_(out), budget_(budget) {}
  bool operator()(const lcdf::Str &k, concurrent_btree::value_type v) {
    if (out_.len() >= budget_) return false;  // stop the walk
    mrx_entry *e = reinterpret_cast<mrx_entry *>(v);
    mrx_val *val = e->val.load(std::memory_order_acquire);
    e->referenced.store(1, std::memory_order_relaxed);
    mrx_chunk_item item;
    item.key.assign(k.data(), k.length());
    item.resident = (val->resident != 0);
    item.tombstone = (val->tombstone != 0);
    if (item.resident && !item.tombstone) {
      mrx_val_copy_out(val, item.value, std::string::npos);
    }
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

// Emit one collected item, filling from RocksDB if the value was not
// resident. Called with NO RCU region held; the fill takes its own
// short region internally via this wrapper.
// @unsafe - may perform a rocksdb read
inline bool mrx_emit_item(mrx_store *s, const mrx_chunk_item &item,
                          oi_scan_callback &cb) {
  if (item.tombstone) return true;  // deleted: skip, do not emit
  if (item.resident) {
    return cb.invoke(item.key.data(), item.key.size(), item.value);
  }
  std::string value;
  bool found = false;
  {
    const auto region = mrx_rcu_region();
    found = mrx_fill(s, lcdf::Str(item.key), value, std::string::npos);
  }
  if (!found) return true;  // vanished under us; skip
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
    }
    if (chunk.len() == 0) return;
    for (size_t i = 0; i < chunk.len(); i++) {
      if (!mrx_emit_item(s, chunk[i], cb)) return;
    }
    cursor = chunk[chunk.len() - 1].key;
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
    if (chunk.len() < MRX_SCAN_CHUNK) return;
  }
}

// ---------------------------------------------------------------------------
// Sweeper: CLOCK reclamation of value bytes.
//
// A rotating cursor walks the keyspace in bounded chunks. A value whose
// reference bit is set gets a second chance — the bit is cleared and
// the value survives this pass — which is what keeps a continuously
// read key resident. Anything else at-or-below the watermark is
// evicted.
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

// One bounded CLOCK pass over the sweeper-private cursor. Returns bytes
// reclaimed.
// @unsafe - tree walk + eviction CAS (sweeper thread only)
inline uint64_t mrx_sweep_chunk(mrx_store *s, size_t budget) {
  const uint64_t before = s->resident_bytes.load(std::memory_order_relaxed);
  bool wrapped = false;
  std::string last;
  {
    const auto region = mrx_rcu_region();
    rusty::Vec<mrx_sweep_item> chunk;
    mrx_sweep_collector c(chunk, budget);
    varkey lower = mrx_key(lcdf::Str(s->sweep_cursor));
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

  if (wrapped) {
    s->sweep_cursor.clear();
  } else {
    s->sweep_cursor = last;
    s->sweep_cursor.push_back('\0');  // strictly after the last key
  }

  const uint64_t after = s->resident_bytes.load(std::memory_order_relaxed);
  return (before > after) ? (before - after) : 0;
}

// @unsafe - thread body. Polls; a pass that reclaims nothing sleeps
// rather than re-scanning, because only flusher progress (a rising
// watermark) can make more values evictable.
inline void mrx_sweeper_loop(mrx_store *s) {
  while (!s->stopping.load(std::memory_order_acquire)) {
    if (!s->clearing.load(std::memory_order_acquire) && mrx_over_capacity(s)) {
      s->sweep_active.store(true, std::memory_order_release);
      // Re-check under the flag: a clear that set `clearing` after our
      // first check now sees sweep_active and waits for us.
      uint64_t freed = 0;
      if (!s->clearing.load(std::memory_order_acquire)) {
        freed = mrx_sweep_chunk(s, MRX_SWEEP_CHUNK);
      }
      s->sweep_active.store(false, std::memory_order_release);
      if (freed == 0) ::usleep(MRX_SWEEP_POLL_US);
    } else {
      ::usleep(MRX_SWEEP_POLL_US);
    }
  }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Establish the invariant: every key in RocksDB gets a resident entry
// with its value evicted. O(keyspace) on a non-empty database, free on
// an empty one. Nothing may run against the store until this completes.
// The version-0 records are at-or-below any watermark, i.e. durable by
// provenance — correct, since their bytes came FROM RocksDB.
// @unsafe - rocksdb iterator + tree inserts
inline void mrx_load_keys(mrx_store *s) {
  const auto region = mrx_rcu_region();
  rocksdb_iterator_t *it = rocksdb_create_iterator(s->db, s->ropts);
  rocksdb_iter_seek_to_first(it);
  while (rocksdb_iter_valid(it)) {
    size_t klen = 0;
    const char *k = rocksdb_iter_key(it, &klen);
    const lcdf::Str key(k, static_cast<int>(klen));
    mrx_val *v = mrx_val_new(0, /*tombstone=*/false, /*resident=*/false,
                             std::string());
    mrx_entry *e = mrx_entry_alloc(v, key);
    if (s->tree->insert_if_absent(
            mrx_key(key), reinterpret_cast<concurrent_btree::value_type>(e))) {
      s->resident_bytes.fetch_add(
          mrx_entry_footprint(e) + mrx_val_footprint(v),
          std::memory_order_relaxed);
      s->floor_bytes.fetch_add(mrx_entry_footprint(e) + sizeof(mrx_val),
                               std::memory_order_relaxed);
    } else {
      mrx_val_drop(v);
      rcu::s_instance.dealloc(e, mrx_entry_footprint(e));
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
  for (uint64_t i = 0; i < MRX_LOG_CAP; i++) {
    s->log.slots[i].seq.store(i, std::memory_order_relaxed);
  }

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

// CONTRACT: close must be externally synchronized — no other thread
// may be inside any operation on this store (including flush()) when
// close begins, or it will touch freed memory. The barrier is retried
// a bounded number of times so a TRANSIENT IO failure does not
// silently discard acked writes; on persistent failure close proceeds
// and the un-discharged tail is lost, which the caller learns only by
// having seen flush() return false (review finding: the result was
// ignored entirely).
// @unsafe - drains, stops the threads, closes RocksDB
inline void mrx_store_close(mrx_store *s) {
  if (s == nullptr) return;
  for (int attempt = 0; attempt < 50; attempt++) {
    if (mrx_flush_barrier(s)) break;
    if (s->stopping.load(std::memory_order_acquire)) break;
    ::usleep(MRX_FLUSH_POLL_US * 10);
  }
  s->stopping.store(true, std::memory_order_release);
  if (s->sweeper.is_some()) {
    s->sweeper.take().unwrap().join();
  }
  if (s->flusher.is_some()) {
    s->flusher.take().unwrap().join();
  }
  // Release any barrier that raced shutdown past the flusher's final
  // notify.
  s->drained_cv.notify_all();
  rocksdb_close(s->db);
  rocksdb_readoptions_destroy(s->ropts);
  rocksdb_writeoptions_destroy(s->wopts);
  rocksdb_options_destroy(s->opts);
  delete s;
}

// @unsafe - estimate; counts tombstones too, so it OVERCOUNTS live
// keys. Pins an RCU region: mbtree::size() walks raw nodes and its
// internal region is compiled out under RcuRespCaller.
inline size_t mrx_size(const concurrent_btree *t) {
  const auto region = scoped_rcu_region();
  return t->size();
}

// TRUNCATE of both tiers, not a cache drop: dropping only the tree
// would break the key-resident invariant while RocksDB still held rows.
// NOT THREAD SAFE (mbtree::clear contract): no concurrent readers,
// writers, or an over-capacity sweeper. Entry/value allocations are
// leaked deliberately, as in oi_mt_clear — a teardown affordance.
// @unsafe - rocksdb iterator + batch delete + tree clear
inline oi_stats_map mrx_clear(mrx_store *s) {
  // The barrier must SUCCEED before the wipe: on a failed barrier the
  // flusher still holds undischarged obligations, and its guaranteed
  // retry would write them into the freshly wiped DB — a RocksDB row
  // with no tree key, resurrected on the next open (review finding).
  // A successful barrier proves the flusher is quiescent: dirty map,
  // stash, floors, and log all empty, and (under the documented
  // no-concurrent-writers contract) nothing new can appear.
  while (!mrx_flush_barrier(s)) {
    if (s->stopping.load(std::memory_order_acquire)) return oi_stats_map();
    ::usleep(MRX_FLUSH_POLL_US);
  }

  // Exclude the sweeper for the duration of the wipe.
  s->clearing.store(true, std::memory_order_release);
  while (s->sweep_active.load(std::memory_order_acquire)) {
    ::usleep(50);
  }

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
  if (err != nullptr) {
    // The wipe itself failed: clearing only the tree would leave
    // RocksDB's rows to be resurrected by the next open. Abort the
    // truncate with both tiers intact (review finding: the error was
    // silently swallowed and the tiers desynchronized).
    rocksdb_free(err);
    s->clearing.store(false, std::memory_order_release);
    return oi_stats_map();
  }

  s->tree->clear();
  s->resident_bytes.store(0, std::memory_order_relaxed);
  s->floor_bytes.store(0, std::memory_order_relaxed);
  s->clearing.store(false, std::memory_order_release);
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
