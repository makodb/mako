// masstree_rocks_bench — does the Masstree write-back cache actually
// earn its complexity over plain RocksDB?
//
// WHAT THIS IS REALLY COMPARING. Plain RocksDB is not "the disk": it
// already buffers writes in a memtable and caches reads in a block
// cache, both in memory. So this is not cache-versus-disk, it is our
// cache versus RocksDB's own caching. That is the honest question, and
// it makes the baseline harder to beat than it first looks.
//
// Both sides are given identical RocksDB options (create_if_missing,
// sync=0), separate directories, the same keys, and the same thread
// count.
//
// Writes are reported twice, because the cache's whole trick is
// deferral:
//   ack        — time until the write is acknowledged
//   ack+flush  — time until it is actually durable, flush included
// The first is the latency the caller sees; the second says whether the
// cache raised total throughput or merely moved the work later.
//
// Usage: masstree_rocks_bench [threads] [ops_per_thread] [value_bytes]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rocksdb/c.h>

#include "mako/storage/masstree_rocks_index.hh"
#include "mako/storage/masstree_ordered_index.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

int g_threads = 16;
int g_ops_per_thread = 50000;
int g_value_bytes = 128;
int g_keyspace = 200000;
// Value-tier ceiling. 0 = unbounded, in which case NOTHING is ever
// evicted and every read is a memory hit - which flatters the cache and
// never exercises the fill path. Pass a capacity to measure misses.
uint64_t g_capacity = 0;
// Skip per-op latency capture. Two clock_gettime calls around a ~300ns
// lookup is itself measurable overhead, so this mode times the whole
// phase instead and reports throughput only.
bool g_no_latency = false;
// Hot-set size for the locality read phase: a small slice of the
// keyspace, which is the case the cache is supposed to win.
int g_hot_keys = 2000;

struct Result {
  double seconds{0};
  uint64_t ops{0};
  double p50_us{0};
  double p99_us{0};

  double ops_per_sec() const { return seconds > 0 ? ops / seconds : 0; }
};

// Keys and values are materialized ONCE, before timing. Building them
// per-op (snprintf + a std::string allocation) cost roughly as much as
// the lookup itself and was silently halving every ops/sec number.
std::vector<std::string> g_keys;
std::vector<std::string> g_vals;

// @safe - deterministic key so all backends see identical work
std::string MakeKey(int i) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%09d", i);
  return std::string(buf);
}

std::string MakeValue(int i) {
  std::string v = "v" + std::to_string(i);
  v.resize(g_value_bytes, 'x');
  return v;
}

// Cheap deterministic PRNG so both backends get the same access
// sequence for a given thread.
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

Result Summarize(std::vector<std::vector<uint64_t>> &lat, double seconds) {
  Result r;
  r.seconds = seconds;
  if (g_no_latency) {
    r.ops = static_cast<uint64_t>(g_threads) * g_ops_per_thread;
    return r;
  }
  std::vector<uint64_t> all;
  for (auto &v : lat) {
    r.ops += v.size();
    all.insert(all.end(), v.begin(), v.end());
  }
  if (!all.empty()) {
    std::sort(all.begin(), all.end());
    r.p50_us = all[all.size() / 2] / 1000.0;
    r.p99_us = all[(all.size() * 99) / 100] / 1000.0;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Plain RocksDB baseline
// ---------------------------------------------------------------------------

struct RawRocks {
  rocksdb_t *db{nullptr};
  rocksdb_options_t *opts{nullptr};
  rocksdb_readoptions_t *ropts{nullptr};
  rocksdb_writeoptions_t *wopts{nullptr};

  // @unsafe - rocksdb C API
  bool Open(const std::string &path) {
    opts = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(opts, 1);
    ropts = rocksdb_readoptions_create();
    wopts = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(wopts, 0);  // same as the cache
    char *err = nullptr;
    db = rocksdb_open(opts, path.c_str(), &err);
    if (err != nullptr) {
      fprintf(stderr, "rocksdb_open failed: %s\n", err);
      rocksdb_free(err);
      return false;
    }
    return true;
  }

  // @unsafe - rocksdb C API
  void Put(const std::string &k, const std::string &v) {
    char *err = nullptr;
    rocksdb_put(db, wopts, k.data(), k.size(), v.data(), v.size(), &err);
    if (err != nullptr) rocksdb_free(err);
  }

  // @unsafe - rocksdb C API
  bool Get(const std::string &k, std::string &out) {
    size_t vlen = 0;
    char *err = nullptr;
    char *v = rocksdb_get(db, ropts, k.data(), k.size(), &vlen, &err);
    if (err != nullptr) {
      rocksdb_free(err);
      return false;
    }
    if (v == nullptr) return false;
    out.assign(v, vlen);
    rocksdb_free(v);
    return true;
  }

  // Force everything to SST, the baseline's equivalent of flush().
  // @unsafe - rocksdb C API
  void Flush() {
    rocksdb_flushoptions_t *fo = rocksdb_flushoptions_create();
    rocksdb_flushoptions_set_wait(fo, 1);
    char *err = nullptr;
    rocksdb_flush(db, fo, &err);
    if (err != nullptr) rocksdb_free(err);
    rocksdb_flushoptions_destroy(fo);
  }

  // @unsafe - rocksdb C API
  void Close() {
    if (db == nullptr) return;
    rocksdb_close(db);
    rocksdb_readoptions_destroy(ropts);
    rocksdb_writeoptions_destroy(wopts);
    rocksdb_options_destroy(opts);
    db = nullptr;
  }
};

// ---------------------------------------------------------------------------
// Phase drivers. Each returns ack-time; the caller adds flush separately.
// ---------------------------------------------------------------------------

template <typename PutFn>
Result RunWrites(PutFn put) {
  std::vector<std::vector<uint64_t>> lat(g_threads);
  std::vector<std::thread> ts;
  const auto start = Clock::now();
  for (int t = 0; t < g_threads; t++) {
    ts.emplace_back([&, t]() {
      lat[t].reserve(g_ops_per_thread);
      Rng rng(t + 1);
      for (int i = 0; i < g_ops_per_thread; i++) {
        const int k = rng.Next() % g_keyspace;
        const std::string &key = g_keys[k];
        const std::string &val = g_vals[k];
        const auto t0 = Clock::now();
        put(key, val);
        lat[t].push_back(
            std::chrono::duration_cast<Nanos>(Clock::now() - t0).count());
      }
    });
  }
  for (auto &th : ts) th.join();
  const double secs =
      std::chrono::duration<double>(Clock::now() - start).count();
  return Summarize(lat, secs);
}

template <typename GetFn>
Result RunReads(GetFn get, int key_range) {
  std::vector<std::vector<uint64_t>> lat(g_threads);
  std::atomic<uint64_t> hits{0};
  std::vector<std::thread> ts;
  const auto start = Clock::now();
  for (int t = 0; t < g_threads; t++) {
    ts.emplace_back([&, t]() {
      lat[t].reserve(g_ops_per_thread);
      Rng rng(t + 101);
      std::string out;
      uint64_t local_hits = 0;
      for (int i = 0; i < g_ops_per_thread; i++) {
        const int k = rng.Next() % key_range;
        const std::string &key = g_keys[k];
        if (g_no_latency) {
          if (get(key, out)) local_hits++;
          continue;
        }
        const auto t0 = Clock::now();
        if (get(key, out)) local_hits++;
        lat[t].push_back(
            std::chrono::duration_cast<Nanos>(Clock::now() - t0).count());
      }
      hits.fetch_add(local_hits);
    });
  }
  for (auto &th : ts) th.join();
  const double secs =
      std::chrono::duration<double>(Clock::now() - start).count();
  Result r = Summarize(lat, secs);
  if (hits.load() == 0) {
    fprintf(stderr, "WARNING: read phase found nothing - results are junk\n");
  }
  return r;
}

void PrintRow(const char *label, const Result &mt, const Result &mrx,
              const Result &raw) {
  // vs-masstree shows how much of raw Masstree's speed the cache layer
  // gives away; vs-rocks shows what the cache buys over the baseline.
  const double of_mt =
      mt.ops_per_sec() > 0 ? mrx.ops_per_sec() / mt.ops_per_sec() : 0;
  const double vs_raw =
      raw.ops_per_sec() > 0 ? mrx.ops_per_sec() / raw.ops_per_sec() : 0;
  printf("%-18s %11.0f %11.0f %11.0f %9.2f %9.2fx %8.2f %8.2f %8.2f\n", label,
         mt.ops_per_sec(), mrx.ops_per_sec(), raw.ops_per_sec(), of_mt, vs_raw,
         mt.p50_us, mrx.p50_us, raw.p50_us);
}

}  // namespace

// @unsafe - benchmark driver: owns raw stores and threads
int main(int argc, char **argv) {
  if (argc > 1) g_threads = atoi(argv[1]);
  if (argc > 2) g_ops_per_thread = atoi(argv[2]);
  if (argc > 3) g_value_bytes = atoi(argv[3]);
  if (argc > 4) g_capacity = strtoull(argv[4], nullptr, 10);
  if (argc > 5) g_no_latency = (atoi(argv[5]) != 0);

  g_keys.reserve(g_keyspace);
  g_vals.reserve(g_keyspace);
  for (int i = 0; i < g_keyspace; i++) {
    g_keys.push_back(MakeKey(i));
    g_vals.push_back(MakeValue(i));
  }

  char tmpl[] = "/tmp/mrx_bench_XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (dir == nullptr) {
    fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }
  const std::string base(dir);
  const std::string mrx_path = base + "/mrx";
  const std::string raw_path = base + "/raw";

  printf("masstree_rocks_bench\n");
  printf("  threads=%d ops/thread=%d value=%dB keyspace=%d hot=%d\n",
         g_threads, g_ops_per_thread, g_value_bytes, g_keyspace, g_hot_keys);
  printf("  total ops per phase: %d\n", g_threads * g_ops_per_thread);
  if (g_capacity == 0) {
    printf("  capacity=UNBOUNDED - nothing is evicted, so every read is a\n");
    printf("  memory hit and the fill path is NEVER exercised.\n");
  } else {
    printf("  capacity=%lluB - reads that miss must fetch from RocksDB\n",
           static_cast<unsigned long long>(g_capacity));
  }
  printf("  baseline is plain RocksDB with identical options (sync=0),\n");
  printf("  which already has a memtable and block cache of its own.\n\n");

  concurrent_btree tree;
  mrx_store *store = mrx_store_open(&tree, mrx_path, g_capacity);
  if (store == nullptr) {
    fprintf(stderr, "mrx_store_open failed\n");
    return 1;
  }
  masstree_rocks_index idx("bench", 1, &tree, store);

  // Control arm: plain Masstree behind the same OrderedIndex trait -
  // no durability, no cache layer. This is the ceiling, and the gap
  // between it and the cache is what our layer costs.
  concurrent_btree mt_tree;
  masstree_ordered_index mt("bench_mt", 2, &mt_tree);

  RawRocks raw;
  if (!raw.Open(raw_path)) return 1;

  // --- write phase -------------------------------------------------
  const auto mrx_w0 = Clock::now();
  Result mrx_write = RunWrites([&](const std::string &k, const std::string &v) {
    idx.put(lcdf::Str(k.data(), static_cast<int>(k.size())), v);
  });
  const double mrx_ack_secs =
      std::chrono::duration<double>(Clock::now() - mrx_w0).count();
  idx.flush();
  const double mrx_durable_secs =
      std::chrono::duration<double>(Clock::now() - mrx_w0).count();

  Result mt_write = RunWrites([&](const std::string &k, const std::string &v) {
    mt.put(lcdf::Str(k.data(), static_cast<int>(k.size())), v);
  });

  const auto raw_w0 = Clock::now();
  Result raw_write = RunWrites(
      [&](const std::string &k, const std::string &v) { raw.Put(k, v); });
  const double raw_ack_secs =
      std::chrono::duration<double>(Clock::now() - raw_w0).count();
  raw.Flush();
  const double raw_durable_secs =
      std::chrono::duration<double>(Clock::now() - raw_w0).count();

  // --- read phases -------------------------------------------------
  Result mrx_hot = RunReads(
      [&](const std::string &k, std::string &out) {
        return idx.get(lcdf::Str(k.data(), static_cast<int>(k.size())), out,
                       std::string::npos);
      },
      g_hot_keys);
  Result mt_hot = RunReads(
      [&](const std::string &k, std::string &out) {
        return mt.get(lcdf::Str(k.data(), static_cast<int>(k.size())), out,
                      std::string::npos);
      },
      g_hot_keys);
  Result raw_hot = RunReads(
      [&](const std::string &k, std::string &out) { return raw.Get(k, out); },
      g_hot_keys);

  Result mrx_uni = RunReads(
      [&](const std::string &k, std::string &out) {
        return idx.get(lcdf::Str(k.data(), static_cast<int>(k.size())), out,
                       std::string::npos);
      },
      g_keyspace);
  Result mt_uni = RunReads(
      [&](const std::string &k, std::string &out) {
        return mt.get(lcdf::Str(k.data(), static_cast<int>(k.size())), out,
                      std::string::npos);
      },
      g_keyspace);
  Result raw_uni = RunReads(
      [&](const std::string &k, std::string &out) { return raw.Get(k, out); },
      g_keyspace);

  // --- report ------------------------------------------------------
  printf("%-18s %11s %11s %11s %9s %10s %8s %8s %8s\n", "phase",
         "masstree/s", "cache/s", "rocks/s", "cache/mt", "cache/rocks",
         "mt p50", "cache p50", "rocks p50");
  printf("%-18s %11s %11s %11s %9s %10s %8s %8s %8s\n", "", "(ceiling)", "",
         "(baseline)", "", "", "(us)", "(us)", "(us)");
  PrintRow("write (ack)", mt_write, mrx_write, raw_write);
  PrintRow("read (hot set)", mt_hot, mrx_hot, raw_hot);
  PrintRow("read (uniform)", mt_uni, mrx_uni, raw_uni);
  printf("\ncache/mt < 1.0 = what the cache layer costs on top of raw\n");
  printf("Masstree (RCU region per op, CLOCK reference-bit store, val\n");
  printf("indirection). Raw Masstree has no durability at all.\n");

  printf("\nwrite wall-clock, ack vs durable:\n");
  printf("  cache   ack %7.3fs   ack+flush %7.3fs   (deferred %.1f%%)\n",
         mrx_ack_secs, mrx_durable_secs,
         mrx_durable_secs > 0
             ? 100.0 * (mrx_durable_secs - mrx_ack_secs) / mrx_durable_secs
             : 0.0);
  printf("  rocks   ack %7.3fs   ack+flush %7.3fs\n", raw_ack_secs,
         raw_durable_secs);
  printf("  end-to-end durable speedup: %.2fx\n",
         mrx_durable_secs > 0 ? raw_durable_secs / mrx_durable_secs : 0.0);
  printf("\ncache resident bytes: %llu\n",
         static_cast<unsigned long long>(idx.resident_bytes()));

  mrx_store_close(store);
  raw.Close();
  std::string cmd = "rm -rf " + base;
  (void)system(cmd.c_str());
  return 0;
}
