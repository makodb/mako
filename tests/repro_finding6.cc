// Minimal reproducer for sanitizer-findings.md Finding 6:
// "Ephemeral threads on concurrent_btree SIGABRT".
//
// Spawns a fresh std::thread, performs a small batch of concurrent_btree
// ops, joins, repeats. The thread spawn rate (~200/s) reliably triggers
// an internal abort within seconds with no diagnostic message on stderr.
//
// Build: gated on MAKO_REPRO_FINDING6=1 in src/masstree/CMakeLists.txt
//   to avoid CTest picking up a binary that crashes by design.
//
// Run under strace to surface the abort site:
//   strace -f -e write,exit_group,tgkill,abort \
//     ./build_local/repro_finding6 2> /tmp/repro.strace
//
// The last `write(2, ...)` to stderr before the tgkill/SIGABRT is the
// INVARIANT / ALWAYS_ASSERT message; we can't see it on plain run
// because the abort fires before stderr is flushed.

#include <stdint.h>
#include <stddef.h>
#include <cstdio>

#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;

volatile mrcu_epoch_type globalepoch = 1;

using TestTree = concurrent_btree;

namespace {

inline u64_varkey K(uint64_t i) { return u64_varkey(i); }
inline TestTree::value_type ToValue(uint64_t v) {
  return reinterpret_cast<TestTree::value_type>(static_cast<uintptr_t>(v));
}

}  // namespace

int main() {
  TestTree tree;
  // Seed so the tree is non-empty when the ephemerals start hitting it.
  for (uint64_t k = 0; k < 64; ++k) tree.insert(K(k), ToValue(k));

  // Tight spawn/join loop. We expect to abort within a few seconds.
  uint64_t base = 1ull << 40;
  for (int spawn = 0; spawn < 100000; ++spawn) {
    std::thread eph([&, spawn]() {
      const uint64_t off = base + static_cast<uint64_t>(spawn) * 64ull;
      for (uint64_t i = 0; i < 64; ++i) {
        tree.insert(K(off + i), ToValue(off + i));
      }
      for (uint64_t i = 0; i < 64; ++i) {
        tree.remove(K(off + i));
      }
    });
    eph.join();
    if ((spawn & 0x1FF) == 0) {
      std::fprintf(stderr, "[repro] spawn=%d survived\n", spawn);
    }
  }
  std::fprintf(stderr, "[repro] reached spawn=100000 without abort!\n");
  return 0;
}
