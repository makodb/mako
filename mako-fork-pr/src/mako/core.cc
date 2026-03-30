#include <unistd.h>

#include "amd64.h"
#include "core.h"
#include "silo_runtime.h"
#include "util.h"

// @safe
extern "C" long sysconf(int name);

using namespace std;
using namespace util;


// =========================================================================
// Helper functions to interact with SiloRuntime
// (Defined here to avoid circular includes)
// =========================================================================

// @safe
SiloRuntime* coreid::get_current_runtime() {
  return SiloRuntime::Current();
}

// @unsafe: raw pointer dereference
int coreid::get_runtime_id(SiloRuntime* runtime) {
  return runtime->id();
}

// @unsafe: raw pointer dereference
unsigned coreid::allocate_from_runtime(SiloRuntime* runtime) {
  return runtime->allocate_core_id();
}

// @unsafe: raw pointer dereference
unsigned coreid::get_core_count_from_runtime(SiloRuntime* runtime) {
  return runtime->core_count();
}

// =========================================================================
// Core ID allocation (now delegates to SiloRuntime)
// =========================================================================

// @unsafe: uses atomic operations and goto
int
coreid::allocate_contiguous_aligned_block(unsigned n, unsigned alignment)
{
  // Delegate to the current runtime
  SiloRuntime* runtime = get_current_runtime();
  return runtime->allocate_contiguous_aligned_block(n, alignment);
}

// @unsafe: raw pointer dereference of runtime
void
coreid::set_core_id(unsigned cid)
{
  SiloRuntime* runtime = get_current_runtime();
  ALWAYS_ASSERT(cid < NMaxCores);
  ALWAYS_ASSERT(cid < runtime->core_count());
  ALWAYS_ASSERT(tl_core_id == -1 || tl_runtime_id != runtime->id());
  tl_core_id = cid;
  tl_runtime_id = runtime->id();
}

// @safe
unsigned
coreid::num_cpus_online()
{
  const long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  ALWAYS_ASSERT(nprocs >= 1);
  return nprocs;
}

// Thread-local storage
__thread int coreid::tl_core_id = -1;
__thread int coreid::tl_runtime_id = -1;