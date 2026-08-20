// mrx_kernel_probe -- how much of each arm's write path ends up in the
// KERNEL, measured under real concurrency.
//
// WHY THIS EXISTS. masstree_rocks_bench runs the C++ cache, raw
// Masstree, the Rust cache and plain RocksDB sequentially in ONE
// process. `strace -c` over that process reports one number per syscall
// for all four arms together, which answers nothing: RocksDB's own
// mutexes swamp everything the two caches do. Splitting the trace after
// the fact -- by thread-count square waves, by clone bursts -- was tried
// and is fragile: under strace the phases stretch by 10-50x, RocksDB
// spawns threads mid-phase, and read phases finish in fewer samples than
// it takes to see them.
//
// So this driver marks the boundaries itself. Every phase brackets its
// work with access("/mrxphase/<name>/{begin,end}"), a syscall that
// nothing else in the process makes and that strace prints with the path
// attached. Adding `access` to -e trace= turns phase attribution into
// exact string matching instead of inference.
//
// It also samples getrusage(RUSAGE_SELF) around each phase. That counter
// accumulates over threads that have already exited, so a phase's worker
// threads are fully accounted for after the join -- which is exactly what
// an external /proc sampler cannot promise.
//
// Setup is deliberately identical to masstree_rocks_bench: same keys,
// same values, same RocksDB options, all four backends opened before any
// phase runs so the background thread population matches. Only the set of
// phases actually executed is selectable, so a single arm can be traced
// on its own.
//
// Usage: mrx_kernel_probe [threads] [ops] [value_bytes] [phases]
//   phases: comma list of cpp_write, rust_write, mtree_write, rocks_write,
//           cpp_flush, rust_flush, cpp_read, rust_read
//           (default cpp_write,rust_write)
//
// BUILD: scripts/build_mrx_kernel_probe.sh -- it compiles and links with
// the flags CMake uses for masstree_rocks_bench (lifted from
// compile_commands.json and build.ninja) WITHOUT adding a CMake target,
// so it does not race a concurrent build in the same build directory.
//
// Trace it with:
//   strace -f -tt -T -o t.trace \
//     -e trace=futex,sched_yield,nanosleep,clock_nanosleep,access,clone,clone3 \
//     build_c22/mrx_kernel_probe 16 50000 128 cpp_write,rust_write

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <rocksdb/c.h>

#include "mako/storage/masstree_rocks_index.hh"
#include "mako/storage/masstree_ordered_index.hh"
#include "mrxdb.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int g_threads = 16;
int g_ops = 50000;
int g_value_bytes = 128;
int g_keyspace = 200000;
int g_hot_keys = 2000;

std::vector<std::string> g_keys;
std::vector<std::string> g_vals;

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

// The phase marker. access() on a path that does not exist: one cheap
// failing syscall, printed by strace with the path, made by nothing else
// in this process.
// @unsafe { raw syscall for trace marking }
void Mark(const char *phase, const char *edge) {
  char path[128];
  snprintf(path, sizeof(path), "/mrxphase/%s/%s", phase, edge);
  (void)::access(path, F_OK);
}

struct Usage {
  long vol{0};
  long invol{0};
};

// Wall-clock epoch seconds, so an external sampler (wchan, /proc) can
// line its samples up with the phase boundaries.
// @unsafe { gettimeofday }
double Now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

// @unsafe { getrusage }
Usage Rusage() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  Usage u;
  u.vol = ru.ru_nvcsw;
  u.invol = ru.ru_nivcsw;
  return u;
}

template <typename Fn>
void RunPhase(const char *name, Fn op, int key_range) {
  const Usage before = Rusage();
  const double wall0 = Now();
  Mark(name, "begin");
  const auto t0 = Clock::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < g_threads; t++) {
    ts.emplace_back([&, t]() {
      Rng rng(t + 1);
      for (int i = 0; i < g_ops; i++) {
        op(static_cast<int>(rng.Next() % key_range));
      }
    });
  }
  for (auto &th : ts) th.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  Mark(name, "end");
  const Usage after = Rusage();
  const double ops = static_cast<double>(g_threads) * g_ops;
  printf("WALL %s %.6f %.6f\n", name, wall0, Now());
  printf("PHASE %-12s %8.4fs %12.0f ops/s  vol_cs %8ld  invol_cs %8ld"
         "  vol/1k %7.3f invol/1k %7.3f\n",
         name, secs, ops / secs, after.vol - before.vol,
         after.invol - before.invol,
         1000.0 * (after.vol - before.vol) / ops,
         1000.0 * (after.invol - before.invol) / ops);
  fflush(stdout);
}

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
    rocksdb_writeoptions_set_sync(wopts, 0);
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
  void Close() {
    if (db == nullptr) return;
    rocksdb_close(db);
    rocksdb_readoptions_destroy(ropts);
    rocksdb_writeoptions_destroy(wopts);
    rocksdb_options_destroy(opts);
    db = nullptr;
  }
};

class RustCache {
 public:
  bool Open(const std::string &path, uint64_t capacity) {
    mrxdb_options_t *o = mrxdb_options_create();
    mrxdb_options_set_capacity_bytes(o, capacity);
    char *err = nullptr;
    db_ = mrxdb_open(o, path.c_str(), &err);
    mrxdb_options_destroy(o);
    if (err != nullptr) {
      fprintf(stderr, "mrxdb_open: %s\n", err);
      mrxdb_free(err);
    }
    return db_ != nullptr;
  }
  void Put(const std::string &k, const std::string &v) {
    mrxdb_put(db_, k.data(), k.size(), v.data(), v.size(), nullptr);
  }
  bool Get(const std::string &k, std::string &out) {
    size_t len = 0;
    char *p = mrxdb_get(db_, k.data(), k.size(), &len, nullptr);
    if (p == nullptr) return false;
    out.assign(p, len);
    mrxdb_free(p);
    return true;
  }
  void Flush() { mrxdb_flush(db_, nullptr); }
  void Close() {
    if (db_ != nullptr) {
      mrxdb_close(db_, nullptr);
      db_ = nullptr;
    }
  }

 private:
  mrxdb_t *db_ = nullptr;
};

bool Want(const std::string &phases, const char *name) {
  return phases.find(name) != std::string::npos;
}

}  // namespace

// @unsafe - benchmark driver: owns raw stores and threads
int main(int argc, char **argv) {
  if (argc > 1) g_threads = atoi(argv[1]);
  if (argc > 2) g_ops = atoi(argv[2]);
  if (argc > 3) g_value_bytes = atoi(argv[3]);
  std::string phases = argc > 4 ? argv[4] : "cpp_write,rust_write";

  g_keys.reserve(g_keyspace);
  g_vals.reserve(g_keyspace);
  for (int i = 0; i < g_keyspace; i++) {
    g_keys.push_back(MakeKey(i));
    g_vals.push_back(MakeValue(i));
  }

  char tmpl[] = "/tmp/mrx_probe_XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (dir == nullptr) {
    fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }
  const std::string base(dir);

  printf("mrx_kernel_probe threads=%d ops/thread=%d value=%dB phases=%s\n",
         g_threads, g_ops, g_value_bytes, phases.c_str());

  concurrent_btree tree;
  mrx_store *store = mrx_store_open(&tree, base + "/mrx", 0);
  if (store == nullptr) {
    fprintf(stderr, "mrx_store_open failed\n");
    return 1;
  }
  masstree_rocks_index idx("bench", 1, &tree, store);

  concurrent_btree mt_tree;
  masstree_ordered_index mt("bench_mt", 2, &mt_tree);

  RawRocks raw;
  if (!raw.Open(base + "/raw")) return 1;

  RustCache rust;
  if (!rust.Open(base + "/rust", 0)) return 1;

  // Phases run in the order named on the command line, so the same
  // binary can be run cpp-first and rust-first to cancel ordering bias.
  size_t pos = 0;
  while (pos < phases.size()) {
    size_t comma = phases.find(',', pos);
    std::string p = phases.substr(pos, comma == std::string::npos
                                           ? std::string::npos
                                           : comma - pos);
    pos = comma == std::string::npos ? phases.size() : comma + 1;
    if (p == "cpp_write") {
      RunPhase("cpp_write", [&](int k) {
        idx.put(lcdf::Str(g_keys[k].data(),
                          static_cast<int>(g_keys[k].size())), g_vals[k]);
      }, g_keyspace);
    } else if (p == "rust_write") {
      RunPhase("rust_write", [&](int k) { rust.Put(g_keys[k], g_vals[k]); },
               g_keyspace);
    } else if (p == "mtree_write") {
      RunPhase("mtree_write", [&](int k) {
        mt.put(lcdf::Str(g_keys[k].data(),
                         static_cast<int>(g_keys[k].size())), g_vals[k]);
      }, g_keyspace);
    } else if (p == "rocks_write") {
      RunPhase("rocks_write", [&](int k) { raw.Put(g_keys[k], g_vals[k]); },
               g_keyspace);
    } else if (p == "cpp_flush") {
      const Usage b = Rusage();
      Mark("cpp_flush", "begin");
      const auto t0 = Clock::now();
      idx.flush();
      const double s = std::chrono::duration<double>(Clock::now() - t0).count();
      Mark("cpp_flush", "end");
      const Usage a = Rusage();
      printf("PHASE %-12s %8.4fs %12s  vol_cs %8ld  invol_cs %8ld\n",
             "cpp_flush", s, "-", a.vol - b.vol, a.invol - b.invol);
      fflush(stdout);
    } else if (p == "rust_flush") {
      const Usage b = Rusage();
      Mark("rust_flush", "begin");
      const auto t0 = Clock::now();
      rust.Flush();
      const double s = std::chrono::duration<double>(Clock::now() - t0).count();
      Mark("rust_flush", "end");
      const Usage a = Rusage();
      printf("PHASE %-12s %8.4fs %12s  vol_cs %8ld  invol_cs %8ld\n",
             "rust_flush", s, "-", a.vol - b.vol, a.invol - b.invol);
      fflush(stdout);
    } else if (p == "cpp_read") {
      RunPhase("cpp_read", [&](int k) {
        std::string out;
        idx.get(lcdf::Str(g_keys[k].data(),
                          static_cast<int>(g_keys[k].size())), out,
                std::string::npos);
      }, g_hot_keys);
    } else if (p == "rust_read") {
      RunPhase("rust_read", [&](int k) {
        std::string out;
        rust.Get(g_keys[k], out);
      }, g_hot_keys);
    } else if (!p.empty()) {
      fprintf(stderr, "unknown phase '%s'\n", p.c_str());
    }
  }

  mrx_store_close(store);
  rust.Close();
  raw.Close();
  std::string cmd = "rm -rf " + base;
  (void)system(cmd.c_str());
  return 0;
}
