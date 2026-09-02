// mrx_write_ablation — where does a C++ cache write actually go?
//
// masstree_rocks_bench compares whole stacks, so a slow write arm could
// be masstree, RocksDB, the flusher, or the cache's own bookkeeping.
// This peels the write path one layer at a time, using the REAL
// internals from masstree_rocks_index.hh (not a model of them), so a
// per-layer cost can be attributed rather than guessed at.
//
// It is the C++ half of crates/mrx-masstree/examples/write_scaling.rs:
// same thread count, same 200k keyspace, same per-thread random keys,
// same 100-byte values, same ack-only accounting. Anything the two
// harnesses do differently is a bug in the comparison, not a finding.
//
//   index         concurrent_btree search + insert_if_absent only.
//                 The ceiling: what the index costs before the cache
//                 layer exists. The word inserted comes from a
//                 PER-THREAD sequence: a shared counter in the baseline
//                 costs ~50 ns/op at 16 threads and would be silently
//                 subtracted back out of every rung above it.
//   index_abi     the same probe+insert, but through mtx_get_or_insert
//                 in libmako.a instead of this file's own instantiation
//                 of the masstree templates. A control, not a rung: it
//                 is the exact call write_scaling's `index` makes, so if
//                 the two ladders disagree here they are not measuring
//                 the same machine code and nothing below is
//                 subtractable. (Measured: 35.0 vs 35.2 vs Rust's 37.0.)
//   index_ver     index + ONLY the seq_cst fetch_add on version_ctr, so
//                 the version draw can be priced with nothing else in
//                 the frame.
//   entry_noacct  + mrx_entry_for and a CAS publish of a fresh mrx_val
//                 (arena alloc, compare_exchange, RCU-deferred free).
//                 No version draw, no byte accounting.
//   entry         + mrx_account_swap, the two relaxed fetch_adds on the
//                 store's single resident_bytes counter. Split out
//                 because a shared counter at 16 threads is a
//                 contention cost, not a per-op cost, and the two look
//                 identical at one thread count.
//   entry_ver     + the version draw: one seq_cst fetch_add on
//                 version_ctr. The real path up to but not including
//                 the announce floor.
//   floors        + the announce floor: seq_cst store before the draw,
//                 release store after the publish, on THIS THREAD'S
//                 writer slot (mrx_writer_for). Matches what
//                 write_scaling's `floors` was meant to measure.
//   floors_shared identical, except all threads share ONE writer slot.
//                 Not a rung of the real path -- a reproduction. It is
//                 what write_scaling's `floors` used to do (one
//                 Arc<WriterSlot> handed to every thread), and keeping
//                 it here prices that mistake in C++ too: +104 ns
//                 against +26 for the same layer on its own slot, which
//                 is why the Rust table read +106 for the announce
//                 floor. Delete it only when nobody needs to be shown
//                 the difference again.
//   log           + mrx_submit: the per-writer batch under its spinlock
//                 and the MPSC ticket log. No flusher, so nothing
//                 drains -- keep threads*ops below MRX_LOG_CAP or the
//                 producers block on a ring that is never recycled.
//   full          masstree_rocks_index::put() against a real store:
//                 flusher thread running, RocksDB underneath.
//
// and two rungs that exist only so the Rust ladder can be subtracted
// from this one. write_scaling's `entry` and `floors` do no byte
// accounting, so comparing them against `entry_ver` and `floors` here
// would charge C++ for a layer Rust never ran:
//
//   ver_noacct    entry_noacct + the version draw   (== Rust `entry`)
//   floors_noacct ver_noacct + the announce floor   (== Rust `floors`)
//
// Usage: mrx_write_ablation [threads] [ops_per_thread] [mode] [keyspace]
//                           [value_bytes]

#include "mako/storage/masstree_rocks_index.hh"

#include "mako/silo_runtime.h"
// Only for the index_abi control: mtx_get_or_insert is the SAME probe +
// insert_if_absent as `index`, but already compiled into libmako.a, so
// running both says whether this file's own instantiation of the
// masstree templates is representative.
#include "mako/storage/mtree_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class Mode {
  kIndex,
  kEntryNoAcct,
  kEntry,
  kEntryVer,
  kFloors,
  kFloorsShared,
  kLog,
  kFull,
  // Two rungs that exist only to line up with write_scaling.rs, whose
  // `entry` and `floors` modes have no byte accounting at all. Comparing
  // Rust's floors-minus-entry against C++'s floors-minus-entry_ver would
  // otherwise charge C++ a layer Rust never ran.
  kVerNoAcct,
  kFloorsNoAcct,
  // ver_noacct with the per-write mrx_val allocation REMOVED: publish a
  // pre-built record from a per-thread ring and never free the displaced
  // one. The mirror image of write_scaling's `alloc_ring`, so the two
  // ladders can both be split into "entry table + publish" and "the
  // record allocation" -- which is what makes the cross-language gap
  // attributable to one of them instead of to Rust's half alone.
  kVerRing,
  // The control for `index`: identical work, but through the mtx_* C ABI
  // in libmako.a -- the exact call the Rust harness's `index` mode makes.
  kIndexAbi,
  // `index` plus ONLY the seq_cst fetch_add on version_ctr, so the
  // version draw can be priced with nothing else in the frame. Pairs
  // with write_scaling's `index_ver`.
  kIndexVer,
  // The same fetch_add, on a counter that lives where RUST'S ladder put
  // it: a 24-byte heap block with the word at offset 16 and only
  // malloc's 16-byte alignment, i.e. the layout of `Arc<AtomicU64>`.
  // `index_ver` above draws from the store's alignas(64) version_ctr
  // inside a ~24 MB allocation. Same instruction, two homes; running
  // both in ONE binary is what separates the price of the instruction
  // from the price of the memory.
  kIndexVerBare,
  // The 2x2's other row: the ABI index (what Rust's `index` actually
  // calls) crossed with both counter homes, so a cross-language
  // difference cannot be blamed on this file's own instantiation of the
  // masstree templates.
  kIndexAbiVer,
  kIndexAbiVerBare,
};

// The counter home Rust's ladder used. `Arc<AtomicU64>` is one heap
// allocation of {strong, weak, data} -- the payload sits at offset 16 of
// a 24-byte block, so it gets malloc's alignment and shares its line
// with whatever the allocator put next to it, NOT a line of its own.
// Reproducing it here is the only way to ask whether the version draw
// costs different amounts in the two languages or merely lives in
// different memory. Measured: it does not. See the 2x2 in the report.
struct mrx_arc_like {
  std::atomic<size_t> strong{1};
  std::atomic<size_t> weak{1};
  std::atomic<uint64_t> data{1};
};
mrx_arc_like *g_bare = nullptr;

mtx_tree *g_abi_tree = nullptr;

// Every layer up to and including kLog runs against a store that was
// never opened: no RocksDB, no flusher, no sweeper. Layer kFull uses a
// real one.
mrx_store *g_store = nullptr;
concurrent_btree *g_tree = nullptr;
masstree_rocks_index *g_idx = nullptr;
// The bug reproduction: ONE writer slot for all threads.
mrx_writer *g_shared_writer = nullptr;
// Per-thread ring of pre-built value records for kVerRing. 64 per thread,
// matching write_scaling's RING.
static const int kRing = 64;
std::vector<std::vector<mrx_val *>> g_ring;
int g_threads = 16;
int g_ops = 50000;
int g_keyspace = 200000;
int g_value_bytes = 100;
// Untimed passes before the timed one. Env: MRX_ABL_WARMUP.
int g_warmup = 0;

// Materialized ONCE, before timing. Building a key per op costs about as
// much as the op.
std::vector<std::string> g_keys;
std::string g_value;

// @safe - 8 bytes, matching write_scaling's "k{:07}" so both harnesses
// give masstree a single-slice key.
std::string MakeKey(int i) {
  char buf[32];
  snprintf(buf, sizeof(buf), "k%07d", i);
  return std::string(buf);
}

// The benchmark's PRNG, seeded t+1 per thread, exactly as
// masstree_rocks_bench does it.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ULL + 1) {}
  uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 32);
  }
};

// @safe
lcdf::Str KeyOf(const std::string &k) {
  return lcdf::Str(k.data(), static_cast<int>(k.size()));
}

// ---------------------------------------------------------------------------
// Layer 1: the index alone.
// ---------------------------------------------------------------------------

// The word is drawn from a PER-THREAD sequence, not a shared counter.
// A shared counter here is not free at 16 threads (measured: ~50 ns/op),
// and putting it in the ladder's baseline subtracts it back out of every
// rung above -- the flaw write_scaling's `index` mode was just fixed for.
// @unsafe - raw tree access under an RCU region
void OpIndex(const std::string &key, uint64_t seq) {
  const auto region = mrx_rcu_region();
  concurrent_btree::value_type v{};
  if (g_tree->search(mrx_key(KeyOf(key)), v)) return;
  g_tree->insert_if_absent(
      mrx_key(KeyOf(key)),
      reinterpret_cast<concurrent_btree::value_type>(seq));
}

// @unsafe - the same probe+insert through the precompiled C ABI
void OpIndexAbi(const std::string &key, uint64_t seq) {
  uint64_t out = 0;
  mtx_get_or_insert(g_abi_tree, key.data(), key.size(), seq, &out);
}

// ---------------------------------------------------------------------------
// Layers 2-5: mrx_write with the upper layers sawn off. The shape below
// is a copy of mrx_write() in masstree_rocks_index.hh -- same region
// scoping, same CAS loop, same exit paths -- with each layer guarded by
// a compile-time constant so a mode costs nothing it does not use.
// ---------------------------------------------------------------------------

// @unsafe - CAS loop over an RCU-protected value chain
template <Mode M>
void OpWrite(const std::string &key, const std::string &value,
             uint64_t &local_ver, int tid = 0, uint64_t seq = 0) {
  constexpr bool kAcct = (M != Mode::kEntryNoAcct && M != Mode::kVerNoAcct &&
                          M != Mode::kFloorsNoAcct && M != Mode::kVerRing);
  constexpr bool kDrawVersion =
      (M == Mode::kEntryVer || M == Mode::kFloors ||
       M == Mode::kFloorsShared || M == Mode::kLog ||
       M == Mode::kVerNoAcct || M == Mode::kFloorsNoAcct ||
       M == Mode::kVerRing);
  constexpr bool kAnnounce =
      (M == Mode::kFloors || M == Mode::kFloorsShared || M == Mode::kLog ||
       M == Mode::kFloorsNoAcct);
  constexpr bool kSubmit = (M == Mode::kLog);
  constexpr bool kRingVal = (M == Mode::kVerRing);

  // ONE outer region, exactly as masstree_rocks_index::put() takes
  // before calling mrx_write. Without it the two regions below are both
  // TOP-LEVEL, and a top-level region is not free: ticker::guard takes
  // this core's spinlock and reads the clock at depth 0, and skips both
  // at depth > 0. Opening two where production opens one charged every
  // rung below `full` for a whole extra region, and `full` (which goes
  // through put(), so one region) did not pay it -- precisely the
  // direction that fakes a cheap top rung.
  const auto outer = mrx_rcu_region();

  mrx_entry *e;
  {
    const auto region = mrx_rcu_region();
    e = mrx_entry_for(g_store, KeyOf(key));
  }
  if (e == nullptr) return;

  mrx_writer *w = nullptr;
  if constexpr (kAnnounce) {
    w = (M == Mode::kFloorsShared) ? g_shared_writer : mrx_writer_for(g_store);
    w->announce.store(g_store->version_ctr.load(std::memory_order_relaxed),
                      std::memory_order_seq_cst);
  }

  for (;;) {
    uint64_t ver = 0;
    bool published = false;
    {
      const auto region = mrx_rcu_region();
      mrx_val *cur = e->val.load(std::memory_order_acquire);
      if constexpr (kDrawVersion) {
        ver = g_store->version_ctr.fetch_add(1, std::memory_order_seq_cst);
      } else {
        ver = ++local_ver;
      }
      mrx_val *nv;
      if constexpr (kRingVal) {
        nv = g_ring[tid][seq % kRing];
      } else {
        nv = mrx_val_new(ver, /*tombstone=*/false, /*resident=*/true, value);
      }
      if (e->val.compare_exchange_weak(cur, nv, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        e->referenced.store(1, std::memory_order_relaxed);
        if constexpr (kAcct) {
          mrx_account_swap(g_store, cur, nv);
        }
        // The ring's records are immortal, exactly as write_scaling's
        // are kept alive by the ring itself, so nothing is freed here.
        if constexpr (!kRingVal) {
          mrx_val_free_rcu(cur);
        }
        published = true;
      } else {
        if constexpr (!kRingVal) {
          mrx_val_drop(nv);
        }
      }
    }
    if (!published) continue;

    if constexpr (kSubmit) {
      mrx_submit(g_store, w, e, ver);  // clears announce
    } else if constexpr (kAnnounce) {
      w->announce.store(UINT64_MAX, std::memory_order_release);
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

std::atomic<int> g_ready{0};
std::atomic<bool> g_go{false};

// One pass over this thread's key sequence. Extracted from Worker only
// so the warm-up can run the identical loop untimed.
// @unsafe - dispatches into the raw-pointer layers
template <Mode M>
void RunOps(const std::vector<std::string> &keys, uint64_t &local_ver,
            int tid = 0) {
  const int n = static_cast<int>(keys.size());
  for (int i = 0; i < n; i++) {
    const std::string &k = keys[i];
    if constexpr (M == Mode::kIndex) {
      OpIndex(k, static_cast<uint64_t>(i) + 1);
    } else if constexpr (M == Mode::kIndexAbi) {
      OpIndexAbi(k, static_cast<uint64_t>(i) + 1);
    } else if constexpr (M == Mode::kIndexVer) {
      OpIndex(k, static_cast<uint64_t>(i) + 1);
      g_store->version_ctr.fetch_add(1, std::memory_order_seq_cst);
    } else if constexpr (M == Mode::kIndexVerBare) {
      OpIndex(k, static_cast<uint64_t>(i) + 1);
      g_bare->data.fetch_add(1, std::memory_order_seq_cst);
    } else if constexpr (M == Mode::kIndexAbiVer) {
      OpIndexAbi(k, static_cast<uint64_t>(i) + 1);
      g_store->version_ctr.fetch_add(1, std::memory_order_seq_cst);
    } else if constexpr (M == Mode::kIndexAbiVerBare) {
      OpIndexAbi(k, static_cast<uint64_t>(i) + 1);
      g_bare->data.fetch_add(1, std::memory_order_seq_cst);
    } else if constexpr (M == Mode::kFull) {
      g_idx->put(KeyOf(k), g_value);
    } else {
      OpWrite<M>(k, g_value, local_ver, tid, static_cast<uint64_t>(i));
    }
  }
}

template <Mode M>
void Worker(int t) {
  // masstree registers lazily on first region entry, but a core ID that
  // arrives mid-measurement is a cost in the timed loop; take it here.
  SiloRuntime::Current()->try_register_current_thread();
  mtx_thread_attach();  // same registration, via the ABI's own bookkeeping
  Rng rng(t + 1);
  // This thread's key sequence, materialized CONTIGUOUSLY in access
  // order. Indexing a shared 200k-string pool at random instead (what
  // masstree_rocks_bench does) adds a cache miss per op that lands on
  // every rung alike, and write_scaling does not pay it -- so matching
  // its layout is what makes the two ladders subtractable.
  std::vector<std::string> keys;
  keys.reserve(g_ops);
  for (int i = 0; i < g_ops; i++) {
    keys.push_back(g_keys[rng.Next() % g_keyspace]);
  }
  uint64_t local_ver = 0;

  // WARM-UP, untimed. At the ladder's usual 50k ops/thread a 16-thread
  // run lasts ~0.03 s, and in 0.03 s the transients ARE the measurement:
  // the masstree starts empty so most ops take the insert path and split
  // nodes, every page of the tree is a first-touch fault, and the boost
  // clock has not finished ramping. Measured, the same rung reports
  // 37.3 ns/op cold and 16.7 ns/op warm -- a 2.2x error, larger than
  // most of the rungs the ladder is trying to resolve, and it is what
  // made the version-draw rung read anywhere from -24 to +77 ns/op.
  // Not usable for `log`, which has no flusher and would fill the ring.
  //
  // AND NOT FREE FOR `full`: a warm-up pass DOUBLES the number of writes
  // the durable path sees, and the C++ arm's cost is a step function of
  // exactly that. Past ~2.5 laps of the 1<<20 ticket ring the flusher is
  // a full ring behind, mrx_log_append starts waiting (1024 sched_yield,
  // then usleep(100)), and ns/op jumps ~37%. Measured at 16 threads:
  // 800k writes -> cpp 225 / rust 228 ns/op (rust/cpp 0.99); 3.2M writes
  // -> cpp 309 / rust 194 (rust/cpp 1.59). Same binaries, same minute.
  // So `full` must be quoted WITH its write count, and warming it moves
  // that count.
  for (int w = 0; w < g_warmup; w++) {
    RunOps<M>(keys, local_ver, t);
  }

  g_ready.fetch_add(1, std::memory_order_release);
  while (!g_go.load(std::memory_order_acquire)) {
    ::sched_yield();
  }

  RunOps<M>(keys, local_ver, t);
}

// CPU-seconds burned during the timed region, divided by wall seconds:
// "how many cores were actually busy". With g_threads workers the ideal is
// g_threads. Materially below it means threads were BLOCKED, which is a
// different bottleneck from executing too many instructions.
struct rusage g_ru0, g_ru1;

double RusageSecs(const struct rusage &r) {
  return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
         1e-6 * static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}

template <Mode M>
double Run() {
  std::vector<std::thread> ts;
  ts.reserve(g_threads);
  for (int t = 0; t < g_threads; t++) {
    ts.emplace_back([t]() { Worker<M>(t); });
  }
  while (g_ready.load(std::memory_order_acquire) < g_threads) {
    ::usleep(200);
  }
  ::getrusage(RUSAGE_SELF, &g_ru0);
  const auto t0 = Clock::now();
  g_go.store(true, std::memory_order_release);
  for (auto &th : ts) th.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  ::getrusage(RUSAGE_SELF, &g_ru1);
  return secs;
}

}  // namespace

// @unsafe - benchmark driver: owns raw stores and threads
int main(int argc, char **argv) {
  if (argc > 1) g_threads = atoi(argv[1]);
  if (argc > 2) g_ops = atoi(argv[2]);
  const std::string mode = (argc > 3) ? argv[3] : "full";
  if (argc > 4) g_keyspace = atoi(argv[4]);
  if (argc > 5) g_value_bytes = atoi(argv[5]);
  if (const char *w = ::getenv("MRX_ABL_WARMUP")) g_warmup = atoi(w);
  if (g_warmup > 0 && mode == "log") {
    fprintf(stderr,
            "refusing to run: `log` has no flusher, so a warm-up pass fills\n"
            "the ring the timed pass then blocks on.\n");
    return 1;
  }

  // The Arc-shaped counter home. `new` for 24 bytes lands in malloc's
  // small-size class: 16-byte alignment, no line of its own, neighbours
  // chosen by the allocator -- exactly the situation Rust's
  // Arc<AtomicU64> is in, and exactly what the store's alignas(64)
  // version_ctr is not. Allocated in every mode so the heap state does
  // not itself differ between the cells of the 2x2.
  g_bare = new mrx_arc_like();

  g_keys.reserve(g_keyspace);
  for (int i = 0; i < g_keyspace; i++) g_keys.push_back(MakeKey(i));
  g_value.assign(g_value_bytes, 'v');

  SiloRuntime::Current()->try_register_current_thread();

  concurrent_btree tree;
  g_tree = &tree;
  const bool abi_mode = (mode == "index_abi" || mode == "index_abi_ver" ||
                         mode == "index_abi_ver_bare");
  if (abi_mode) {
    mtx_thread_attach();
    g_abi_tree = mtx_create();
    if (g_abi_tree == nullptr) {
      fprintf(stderr, "mtx_create failed\n");
      return 1;
    }
  }

  std::string db_dir;
  if (mode == "full") {
    char tmpl[] = "/tmp/mrx_abl_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == nullptr) {
      fprintf(stderr, "mkdtemp failed\n");
      return 1;
    }
    db_dir = std::string(dir) + "/db";
    g_store = mrx_store_open(&tree, db_dir, /*capacity=*/0);
    if (g_store == nullptr) {
      fprintf(stderr, "mrx_store_open failed\n");
      return 1;
    }
    g_idx = new masstree_rocks_index("abl", 1, &tree, g_store);
  } else if (mode != "index") {
    // A store that was never opened: the write path's data structures
    // with no RocksDB and no background threads.
    g_store = new mrx_store();
    g_store->tree = &tree;
    for (uint64_t i = 0; i < MRX_LOG_CAP; i++) {
      g_store->log.slots[i].seq.store(i, std::memory_order_relaxed);
    }
    // The shared slot comes out of the same registry, so it is the same
    // kind of memory as a per-thread one -- only the sharing differs.
    g_shared_writer = mrx_writer_for(g_store);
  }

  if (mode == "ver_ring") {
    // Built on the main thread, as write_scaling builds its rings before
    // the barrier. The arena is per-thread, so allocating here and using
    // them from workers is exactly what the Rust ring does too.
    const auto region = mrx_rcu_region();
    g_ring.resize(g_threads);
    for (int t = 0; t < g_threads; t++) {
      for (int r = 0; r < kRing; r++) {
        g_ring[t].push_back(mrx_val_new(static_cast<uint64_t>(t) * kRing + r,
                                        false, true, g_value));
      }
    }
  }

  if (mode == "log") {
    const uint64_t total = static_cast<uint64_t>(g_threads) * g_ops;
    if (total > MRX_LOG_CAP) {
      fprintf(stderr,
              "refusing to run: %llu tickets > MRX_LOG_CAP %llu and there is\n"
              "no flusher to recycle slots, so the producers would block on a\n"
              "ring that never drains.\n",
              static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(MRX_LOG_CAP));
      return 1;
    }
  }

  double secs = 0;
  if (mode == "index") {
    secs = Run<Mode::kIndex>();
  } else if (mode == "index_abi") {
    secs = Run<Mode::kIndexAbi>();
  } else if (mode == "index_ver") {
    secs = Run<Mode::kIndexVer>();
  } else if (mode == "index_ver_bare") {
    secs = Run<Mode::kIndexVerBare>();
  } else if (mode == "index_abi_ver") {
    secs = Run<Mode::kIndexAbiVer>();
  } else if (mode == "index_abi_ver_bare") {
    secs = Run<Mode::kIndexAbiVerBare>();
  } else if (mode == "entry_noacct") {
    secs = Run<Mode::kEntryNoAcct>();
  } else if (mode == "entry") {
    secs = Run<Mode::kEntry>();
  } else if (mode == "entry_ver") {
    secs = Run<Mode::kEntryVer>();
  } else if (mode == "floors") {
    secs = Run<Mode::kFloors>();
  } else if (mode == "floors_shared") {
    secs = Run<Mode::kFloorsShared>();
  } else if (mode == "ver_noacct") {
    secs = Run<Mode::kVerNoAcct>();
  } else if (mode == "floors_noacct") {
    secs = Run<Mode::kFloorsNoAcct>();
  } else if (mode == "ver_ring") {
    secs = Run<Mode::kVerRing>();
  } else if (mode == "log") {
    secs = Run<Mode::kLog>();
  } else if (mode == "full") {
    secs = Run<Mode::kFull>();
  } else {
    fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 1;
  }

  const double total = static_cast<double>(g_threads) * g_ops;
  const double cpu =
      (RusageSecs(g_ru1) - RusageSecs(g_ru0)) / (secs > 0 ? secs : 1);
  // ns/op is aggregate inverse throughput (1e9 / ops per sec), which is
  // what write_scaling's table reports -- NOT per-thread latency.
  printf(
      "threads=%3d mode=%-13s keyspace=%8d %10.0f ops/s %8.1f ns/op "
      "cpu=%5.2f (%.3fs)\n",
      g_threads, mode.c_str(), g_keyspace, total / secs, secs * 1e9 / total,
      cpu, secs);
  fflush(stdout);
  // Deliberately no teardown for the ablation stores: mrx_store_close
  // wants a drained log and a live RocksDB, and neither exists for a
  // store that was never opened. The process is about to exit.
  ::_exit(0);
}
