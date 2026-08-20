// mrx_callgrind — instruction-level attribution for ONE write, C++ vs Rust.
//
// perf(1) is unavailable on this machine (perf_event_paranoid=4), so the
// only way to get sub-layer attribution is a simulator. This driver runs
// BOTH cache implementations in one process, SINGLE-THREADED, with
// callgrind instrumentation toggled on only around each measured loop
// and a named dump per phase.
//
// WHAT THIS CAN AND CANNOT SAY. valgrind serialises threads, so nothing
// here measures contention. It measures WORK PER OPERATION: instructions
// retired, data references, and simulated D1/LL misses for one write (or
// read) on each side. If one side executes materially more instructions,
// that is a gap no lock tuning can close.
//
// Usage: mrx_callgrind [ops] [value_bytes] [arm]
//   arm = both (default) | cpp | rust
//
// `both` runs the two arms back to back in one process, which is the
// honest comparison: same allocator state, same simulated caches, same
// ordering. `cpp` / `rust` exist so the BACKGROUND threads can be
// attributed too — with only one cache open, every non-main thread in
// the process belongs to that arm.

#include "mako/storage/masstree_rocks_index.hh"
#include "mako/silo_runtime.h"
#include "mrxdb.h"

#include <valgrind/callgrind.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

int g_ops = 20000;
int g_value_bytes = 128;

std::vector<std::string> g_keys;
std::string g_value;

std::string MakeKey(int i) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%09d", i);
  return std::string(buf);
}

// The same deterministic PRNG masstree_rocks_bench uses, so the access
// sequence is the one the throughput numbers came from.
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

masstree_rocks_index *g_cpp = nullptr;
mrxdb_t *g_rust = nullptr;

void CppPut(const std::string &k) {
  g_cpp->put(lcdf::Str(k.data(), static_cast<int>(k.size())), g_value);
}

void RustPut(const std::string &k) {
  mrxdb_put(g_rust, k.data(), k.size(), g_value.data(), g_value.size(),
            nullptr);
}

bool CppGet(const std::string &k, std::string &out) {
  return g_cpp->get(lcdf::Str(k.data(), static_cast<int>(k.size())), out,
                    std::string::npos);
}

bool RustGet(const std::string &k, std::string &out) {
  size_t len = 0;
  char *p = mrxdb_get(g_rust, k.data(), k.size(), &len, nullptr);
  if (p == nullptr) return false;
  out.assign(p, len);
  mrxdb_free(p);
  return true;
}

}  // namespace

// @unsafe - benchmark driver: owns raw stores and a C ABI handle
int main(int argc, char **argv) {
  if (argc > 1) g_ops = atoi(argv[1]);
  if (argc > 2) g_value_bytes = atoi(argv[2]);
  const std::string arm = (argc > 3) ? argv[3] : "both";
  // "phases" = the six per-op dumps. "flusher" = one instrumented window
  // covering N writes AND the barrier that makes them durable, so the
  // background threads' exit dumps price the writeback path per write.
  const std::string mode = (argc > 4) ? argv[4] : "phases";
  const bool do_cpp = (arm == "both" || arm == "cpp");
  const bool do_rust = (arm == "both" || arm == "rust");
  if (!do_cpp && !do_rust) {
    fprintf(stderr, "arm must be both|cpp|rust\n");
    return 1;
  }

  // Build marker: proves the binary under valgrind is THIS source.
  printf("mrx_callgrind BUILD_MARKER=cg7 ops=%d value=%dB arm=%s\n", g_ops,
         g_value_bytes, arm.c_str());
  fflush(stdout);

  const int n = g_ops;
  g_keys.reserve(2 * n);
  for (int i = 0; i < 2 * n; i++) g_keys.push_back(MakeKey(i));
  g_value.assign(g_value_bytes, 'v');

  SiloRuntime::Current()->try_register_current_thread();

  char tmpl[] = "/tmp/mrx_cg_XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (dir == nullptr) {
    fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }
  const std::string base(dir);

  concurrent_btree tree;
  if (do_cpp) {
    mrx_store *store = mrx_store_open(&tree, base + "/cpp", /*capacity=*/0);
    if (store == nullptr) {
      fprintf(stderr, "mrx_store_open failed\n");
      return 1;
    }
    g_cpp = new masstree_rocks_index("cg", 1, &tree, store);
  }

  if (do_rust) {
    mrxdb_options_t *o = mrxdb_options_create();
    mrxdb_options_set_capacity_bytes(o, 0);
    char *err = nullptr;
    const std::string rust_path = base + "/rust";
    g_rust = mrxdb_open(o, rust_path.c_str(), &err);
    mrxdb_options_destroy(o);
    if (g_rust == nullptr) {
      fprintf(stderr, "mrxdb_open failed: %s\n", err ? err : "(null)");
      return 1;
    }
  }

  // --- prefill, uninstrumented -------------------------------------
  // Keys [0, n) exist on both sides, so the overwrite and read phases
  // below never take an insert path.
  for (int i = 0; i < n; i++) {
    if (do_cpp) CppPut(g_keys[i]);
    if (do_rust) RustPut(g_keys[i]);
  }
  printf("prefilled %d keys\n", n);
  fflush(stdout);

  std::string out;

  if (mode == "flusher") {
    // Drain the prefill first, so the instrumented window below contains
    // exactly N writes' worth of writeback and nothing carried over.
    if (do_cpp) g_cpp->flush();
    if (do_rust) mrxdb_flush(g_rust, nullptr);
    printf("prefill drained\n");
    fflush(stdout);

    Rng r(7);
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = 0; i < n; i++) {
      if (do_cpp) CppPut(g_keys[r.Next() % n]);
      if (do_rust) RustPut(g_keys[r.Next() % n]);
    }
    if (do_cpp) g_cpp->flush();
    if (do_rust) mrxdb_flush(g_rust, nullptr);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("main_writes_plus_barrier");
    printf("done\n");
    fflush(stdout);
    // Fall through to _exit so the background threads dump what they did
    // inside that window.
    _exit(0);
  }

  // --- phase: overwrite (the steady-state write) --------------------
  if (do_cpp) {
    Rng r(7);
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = 0; i < n; i++) CppPut(g_keys[r.Next() % n]);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("cpp_overwrite");
  }
  if (do_rust) {
    Rng r(7);
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = 0; i < n; i++) RustPut(g_keys[r.Next() % n]);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("rust_overwrite");
  }

  // --- phase: read (hit, resident) ----------------------------------
  if (do_cpp) {
    Rng r(11);
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = 0; i < n; i++) CppGet(g_keys[r.Next() % n], out);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("cpp_read");
  }
  if (do_rust) {
    Rng r(11);
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = 0; i < n; i++) RustGet(g_keys[r.Next() % n], out);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("rust_read");
  }

  // --- phase: insert (first touch of a key) -------------------------
  // Keys [n, 2n) are fresh, so this is the intern path on both sides.
  if (do_cpp) {
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = n; i < 2 * n; i++) CppPut(g_keys[i]);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("cpp_insert");
  }
  if (do_rust) {
    CALLGRIND_START_INSTRUMENTATION;
    for (int i = n; i < 2 * n; i++) RustPut(g_keys[i]);
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT("rust_insert");
  }

  printf("done\n");
  fflush(stdout);
  // No close: teardown flushes RocksDB and joins the flusher threads,
  // which is slow under valgrind and lands outside every measured phase
  // anyway.
  _exit(0);
}
