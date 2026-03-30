#ifndef _NDB_ALLOCATOR_H_
#define _NDB_ALLOCATOR_H_

#include <cstdint>
#include <iterator>
#include <mutex>

#include "util.h"
#include "core.h"
#include "macros.h"
#include "spinlock.h"

// Forward declaration
class SiloRuntime;

/**
 * allocator - Memory allocation facade
 *
 * This class provides static methods for backward compatibility.
 * All methods now delegate to the current SiloRuntime's allocator.
 *
 * Each SiloRuntime has its own memory region for complete isolation
 * between shards in multi-shard single-process mode.
 */
class allocator {
public:

  // our allocator doesn't let allocations exceed maxpercore over a single core
  //
  // Initialize can be called many times- but only the first call has effect.
  //
  // w/o calling Initialize(), behavior for this class is undefined
  // Now delegates to SiloRuntime::Current()->InitializeAllocator()
  static void Initialize(size_t ncpus, size_t maxpercore);

  static void DumpStats();

  // returns an arena linked-list
  // Delegates to SiloRuntime::Current()->AllocateArenas()
  static void *
  AllocateArenas(size_t cpu, size_t sz);

  // allocates nhugepgs * hugepagesize contiguous bytes from CPU's region and
  // returns the raw, unmanaged pointer.
  //
  // Note that memory returned from here cannot be released back to the
  // allocator, so this should only be used for data structures which live
  // throughput the duration of the system (ie log buffers)
  // Delegates to SiloRuntime::Current()->AllocateUnmanaged()
  static void *
  AllocateUnmanaged(size_t cpu, size_t nhugepgs);

  // Delegates to SiloRuntime::Current()->ReleaseArenas()
  static void
  ReleaseArenas(void **arenas);

  static const size_t LgAllocAlignment = 4; // all allocations aligned to 2^4 = 16
  static const size_t AllocAlignment = 1 << LgAllocAlignment;
  static const size_t MAX_ARENAS = 32;

  static inline std::pair<size_t, size_t>
  ArenaSize(size_t sz)
  {
    const size_t allocsz = util::round_up<size_t, LgAllocAlignment>(sz);
    const size_t arena = allocsz / AllocAlignment - 1;
    return std::make_pair(allocsz, arena);
  }

  // slow, but only needs to be called on initialization
  // Delegates to SiloRuntime::Current()->FaultRegion()
  static void
  FaultRegion(size_t cpu);

  // returns true if managed by current runtime's allocator, false otherwise
  // Delegates to SiloRuntime::Current()->ManagesPointer()
  static bool
  ManagesPointer(const void *p);

  // assumes p is managed by this allocator- returns the CPU from which this pointer
  // was allocated. Delegates to SiloRuntime::Current()->PointerToCpu()
  static size_t
  PointerToCpu(const void *p);

#ifdef MEMCHECK_MAGIC
  struct pgmetadata {
    uint32_t unit_; // 0-indexed
  } PACKED;

  // returns nullptr if p is not managed, or has not been allocated yet.
  // p does not have to be properly aligned
  static const pgmetadata *
  PointerToPgMetadata(const void *p);
#endif

  static size_t
  GetPageSize()
  {
    static const size_t sz = GetPageSizeImpl();
    return sz;
  }

  static size_t
  GetHugepageSize()
  {
    static const size_t sz = GetHugepageSizeImpl();
    return sz;
  }

private:
  static size_t GetPageSizeImpl();
  static size_t GetHugepageSizeImpl();
  static bool UseMAdvWillNeed();

  // Legacy: These are kept for backward compatibility but are no longer used.
  // All state is now in SiloRuntime::AllocatorState.
  // DO NOT USE - will be removed in a future version.
  static void *g_memstart;
  static void *g_memend;
  static size_t g_ncpus;
  static size_t g_maxpercore;
};

#endif /* _NDB_ALLOCATOR_H_ */
