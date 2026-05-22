// Minimal libnuma compatibility header for macOS (and other non-Linux builds).
//
// This project uses libnuma on Linux for CPU/node placement and node-local
// allocations. macOS has no libnuma equivalent, and our CI/test workloads do
// not require NUMA-aware behavior. Provide stubs that preserve behavior
// (single NUMA node) and allow the code to compile.
//
// Linux builds defer to the system <numa.h> via #include_next.
#pragma once

#if defined(__linux__)

#if defined(__has_include_next)
#if __has_include_next(<numa.h>)
#include_next <numa.h>
#else
#error "Expected system <numa.h> on Linux after compat include path"
#endif
#endif

#else // !__linux__

#include <cstdint>
#include <cstdlib>

struct bitmask {
  unsigned long size;
  unsigned long* maskp;
};

static inline int numa_available(void) { return -1; }
static inline int numa_max_node(void) { return 0; }
static inline int numa_num_configured_nodes(void) { return 1; }
static inline int numa_node_of_cpu(int /*cpu*/) { return 0; }
static inline int numa_run_on_node(int /*node*/) { return 0; }
static inline int numa_run_on_node_mask(void* /*mask*/) { return 0; }

static inline bitmask* numa_allocate_nodemask(void) {
  auto* bm = static_cast<bitmask*>(std::malloc(sizeof(bitmask)));
  if (!bm) return nullptr;
  bm->size = 0;
  bm->maskp = nullptr;
  return bm;
}

static inline void numa_bitmask_setbit(bitmask* /*bm*/, unsigned /*n*/) {}
static inline void numa_interleave_memory(void* /*start*/, std::size_t /*size*/, bitmask* /*mask*/) {}
static inline void numa_free_nodemask(bitmask* bm) { std::free(bm); }

static inline long long numa_node_size64(int /*node*/, long long* freep) {
  if (freep) *freep = 0;
  return 0;
}

static inline void* numa_alloc_onnode(std::size_t size, int /*node*/) {
  return std::malloc(size);
}

static inline void numa_free(void* ptr, std::size_t /*size*/) {
  std::free(ptr);
}

static inline int numa_set_preferred(int /*node*/) { return 0; }

#endif
