/**
 * allocator.cc
 *
 * Facade for per-runtime memory allocation.
 * All methods delegate to SiloRuntime::Current() for complete
 * memory isolation between shards.
 */

#include <sys/mman.h>
#include <unistd.h>
#include <map>
#include <iostream>
#include <cstring>
#include <numa.h>

#include "allocator.h"
#include "silo_runtime.h"
#include "spinlock.h"
#include "lockguard.h"
#include "static_vector.h"
#include "counter.h"

using namespace util;

// Legacy static members - kept for backward compatibility but not used
void *allocator::g_memstart = nullptr;
void *allocator::g_memend = nullptr;
size_t allocator::g_ncpus = 0;
size_t allocator::g_maxpercore = 0;

#ifdef MEMCHECK_MAGIC
// @unsafe: uses pointer arithmetic and casts
const allocator::pgmetadata *
allocator::PointerToPgMetadata(const void *p)
{
  // Not implemented for per-runtime allocator
  // Would need to check all runtimes' allocators
  return nullptr;
}
#endif

// @unsafe: uses fopen, getline, and raw pointers
size_t
allocator::GetHugepageSizeImpl()
{
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return 2 * 1024 * 1024;  // Default 2MB

  char *linep = nullptr;
  size_t n = 0;
  static const char *key = "Hugepagesize:";
  static const int keylen = strlen(key);
  size_t size = 0;

  while (getline(&linep, &n, f) > 0) {
    if (strstr(linep, key) == linep) {
      size = atol(linep + keylen) * 1024;
      break;
    }
  }
  free(linep);
  fclose(f);

  if (!size) size = 2 * 1024 * 1024;  // Default 2MB
  return size;
}

// @unsafe: uses sysconf
size_t
allocator::GetPageSizeImpl()
{
  return sysconf(_SC_PAGESIZE);
}

// @unsafe: uses getenv
bool
allocator::UseMAdvWillNeed()
{
  static const char *px = getenv("DISABLE_MADV_WILLNEED");
  static const std::string s = px ? to_lower(px) : "";
  static const bool use_madv = !(s == "1" || s == "true");
  return use_madv;
}

// @unsafe: uses mmap and numa operations
void
allocator::Initialize(size_t ncpus, size_t maxpercore)
{
  // Delegate to current runtime's allocator
  SiloRuntime::Current()->InitializeAllocator(ncpus, maxpercore);
}

// @unsafe: calls undeclared SiloRuntime::DumpStats
void
allocator::DumpStats()
{
  // Delegate to current runtime's allocator
  SiloRuntime::Current()->DumpStats();
}

// @unsafe: calls unsafe initialize_page
void *
allocator::AllocateArenas(size_t cpu, size_t arena)
{
  // Delegate to current runtime's allocator
  return SiloRuntime::Current()->AllocateArenas(cpu, arena);
}

// @unsafe: returns raw pointer from runtime
void *
allocator::AllocateUnmanaged(size_t cpu, size_t nhugepgs)
{
  // Delegate to current runtime's allocator
  return SiloRuntime::Current()->AllocateUnmanaged(cpu, nhugepgs);
}

// @unsafe: uses reinterpret_cast with raw pointers
void
allocator::ReleaseArenas(void **arenas)
{
  // Delegate to current runtime's allocator
  SiloRuntime::Current()->ReleaseArenas(arenas);
}

// @unsafe: uses mmap and numa operations
void
allocator::FaultRegion(size_t cpu)
{
  // Delegate to current runtime's allocator
  SiloRuntime::Current()->FaultRegion(cpu);
}

// @unsafe: uses raw pointer check
bool
allocator::ManagesPointer(const void *p)
{
  // Delegate to current runtime's allocator
  return SiloRuntime::Current()->ManagesPointer(p);
}

// @unsafe: uses raw pointer arithmetic
size_t
allocator::PointerToCpu(const void *p)
{
  // Delegate to current runtime's allocator
  return SiloRuntime::Current()->PointerToCpu(p);
}