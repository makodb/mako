/**
 * silo_runtime.cc
 *
 * Implementation of per-site SiloRuntime.
 * @safe - Uses RustyCpp smart pointers for memory safety
 */

#include "silo_runtime.h"
#include "ticker.h"
#include "rcu.h"
#include "amd64.h"  // For nop_pause()
#include "util.h"   // For slow_round_up()
#include "lockguard.h"
#include "counter.h"

#include <sys/mman.h>
#include <numa.h>
#include <iostream>
#include <cstring>

using namespace util;

// Thread-local runtime pointer
// @unsafe - Thread-local storage with raw pointer for performance
// Safety invariant: pointed-to SiloRuntime must outlive the thread
thread_local SiloRuntime* tl_silo_runtime = nullptr;

// Static member initialization
std::atomic<int> SiloRuntime::s_next_runtime_id_{0};
rusty::Arc<SiloRuntime> SiloRuntime::s_global_default_{nullptr};
std::mutex SiloRuntime::s_global_mutex_;

// Event counter for allocator usage
static event_counter evt_silo_runtime_region_usage("silo_runtime_region_usage_bytes");

// @safe
SiloRuntime::SiloRuntime()
    : runtime_id_(s_next_runtime_id_.fetch_add(1, std::memory_order_relaxed)),
      // @unsafe {
      // MasstreeContext is non-copyable/non-movable, so use raw pointer constructor
      masstree_ctx_(new MasstreeContext()) {
      // }
    // MasstreeContext is created inline and owned by this SiloRuntime
}

// @safe
SiloRuntime::~SiloRuntime() {
    // Ticker and RCU are destroyed by unique_ptr automatically
    // Allocator memory is not unmapped (lives for process duration)
}

// @safe
// @lifetime: owned
rusty::Arc<SiloRuntime> SiloRuntime::Create() {
    // Create new SiloRuntime wrapped in Arc for thread-safe sharing
    return rusty::Arc<SiloRuntime>::make();
}

// @safe
void SiloRuntime::BindCurrentThread(SiloRuntime* runtime) {
    // @unsafe {
    // Store raw pointer in thread-local storage
    // Safety: Caller must ensure runtime outlives all threads that use it
    tl_silo_runtime = runtime;
    // }

    // Also bind the MasstreeContext for this runtime
    if (runtime) {
        MasstreeContext::BindCurrentThread(runtime->masstree_ctx_.get());
    }
}

// @safe
// @lifetime: &'static
SiloRuntime* SiloRuntime::Current() {
    if (tl_silo_runtime) {
        return tl_silo_runtime;
    }
    // Return global default for backward compatibility
    return GlobalDefault();
}

// @safe
// @lifetime: &'static
SiloRuntime* SiloRuntime::GlobalDefault() {
    // Fast path: already initialized
    if (s_global_default_) {
        return s_global_default_.get_mut();
    }

    // Slow path: thread-safe lazy initialization
    std::lock_guard<std::mutex> lock(s_global_mutex_);

    // Double-check after acquiring lock
    if (!s_global_default_) {
        s_global_default_ = Create();
    }

    return s_global_default_.get_mut();
}

// =========================================================================
// Core ID Management
// =========================================================================

// @safe
unsigned SiloRuntime::allocate_core_id() {
    unsigned id = core_count_.fetch_add(1, std::memory_order_acq_rel);
    ALWAYS_ASSERT(id < NMaxCores);
    return id;
}

// @safe
int SiloRuntime::allocate_contiguous_aligned_block(unsigned n, unsigned alignment) {
    using namespace util;
retry:
    unsigned current = core_count_.load(std::memory_order_acquire);
    const unsigned rounded = slow_round_up(current, alignment);
    const unsigned replace = rounded + n;
    if (unlikely(replace > NMaxCores))
        return -1;
    if (!core_count_.compare_exchange_strong(current, replace, std::memory_order_acq_rel)) {
        nop_pause();
        goto retry;
    }
    return rounded;
}

// =========================================================================
// Allocator Management
// =========================================================================

static size_t GetHugepageSize() {
    static size_t sz = 0;
    if (sz) return sz;

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 2 * 1024 * 1024;  // Default 2MB

    char *linep = nullptr;
    size_t n = 0;
    static const char *key = "Hugepagesize:";
    static const int keylen = strlen(key);

    while (getline(&linep, &n, f) > 0) {
        if (strstr(linep, key) == linep) {
            sz = atol(linep + keylen) * 1024;
            break;
        }
    }
    free(linep);
    fclose(f);

    if (!sz) sz = 2 * 1024 * 1024;  // Default 2MB
    return sz;
}

static bool UseMAdvWillNeed() {
    static const char *px = getenv("DISABLE_MADV_WILLNEED");
    static const std::string s = px ? to_lower(px) : "";
    static const bool use_madv = !(s == "1" || s == "true");
    return use_madv;
}

void SiloRuntime::InitializeAllocator(size_t ncpus, size_t maxpercore) {
    lock_guard<spinlock> l(alloc_.init_lock);
    if (alloc_.initialized)
        return;

    ALWAYS_ASSERT(!alloc_.memstart);
    ALWAYS_ASSERT(!alloc_.memend);

    const size_t hugepgsize = GetHugepageSize();

    // Round maxpercore to nearest hugepage size
    maxpercore = slow_round_up(maxpercore, hugepgsize);

    alloc_.ncpus = ncpus;
    alloc_.maxpercore = maxpercore;

    // mmap the entire region (PROT_NONE - just reserve address space)
    void * const x = mmap(nullptr, ncpus * maxpercore + hugepgsize,
        PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (x == MAP_FAILED) {
        perror("SiloRuntime::InitializeAllocator: mmap failed");
        ALWAYS_ASSERT(false);
    }

    void * const endpx = (void *) ((uintptr_t)x + ncpus * maxpercore + hugepgsize);
    std::cerr << "SiloRuntime[" << runtime_id_ << "]::InitializeAllocator()" << std::endl
              << "  hugepgsize: " << hugepgsize << std::endl
              << "  mmap() region [" << x << ", " << endpx << ")" << std::endl;

    alloc_.memstart = reinterpret_cast<void *>(util::iceil(uintptr_t(x), hugepgsize));
    alloc_.memend = reinterpret_cast<char *>(alloc_.memstart) + (ncpus * maxpercore);

    ALWAYS_ASSERT(!(reinterpret_cast<uintptr_t>(alloc_.memstart) % hugepgsize));

    // Initialize per-cpu regions using unique_ptr (regionctx is non-copyable/non-movable)
    alloc_.regions.reserve(ncpus);
    for (size_t i = 0; i < ncpus; i++) {
        alloc_.regions.push_back(std::make_unique<regionctx>());
        alloc_.regions[i]->region_begin =
            reinterpret_cast<char *>(alloc_.memstart) + (i * maxpercore);
        alloc_.regions[i]->region_end =
            reinterpret_cast<char *>(alloc_.memstart) + ((i + 1) * maxpercore);
        std::cerr << "  runtime" << runtime_id_ << " cpu" << i
                  << " owns [" << alloc_.regions[i]->region_begin
                  << ", " << alloc_.regions[i]->region_end << ")" << std::endl;
    }

    alloc_.initialized = true;
}

size_t SiloRuntime::PointerToCpu(const void *p) const {
    ALWAYS_ASSERT(p >= alloc_.memstart);
    ALWAYS_ASSERT(p < alloc_.memend);
    const size_t ret =
        (reinterpret_cast<const char *>(p) -
         reinterpret_cast<const char *>(alloc_.memstart)) / alloc_.maxpercore;
    ALWAYS_ASSERT(ret < alloc_.ncpus);
    return ret;
}

static void *
initialize_page(void *page, const size_t pagesize, const size_t unit)
{
    INVARIANT(((uintptr_t)page % pagesize) == 0);

    void *first = (void *)util::iceil((uintptr_t)page, (uintptr_t)unit);
    INVARIANT((uintptr_t)first + unit <= (uintptr_t)page + pagesize);
    void **p = (void **)first;
    void *next = (void *)((uintptr_t)p + unit);
    while ((uintptr_t)next + unit <= (uintptr_t)page + pagesize) {
        INVARIANT(((uintptr_t)p % unit) == 0);
        *p = next;
        p = (void **)next;
        next = (void *)((uintptr_t)next + unit);
    }
    INVARIANT(((uintptr_t)p % unit) == 0);
    *p = nullptr;
    return first;
}

void* SiloRuntime::AllocateArenas(size_t cpu, size_t arena) {
    INVARIANT(cpu < alloc_.ncpus);
    INVARIANT(arena < MAX_ARENAS);
    INVARIANT(alloc_.memstart);
    INVARIANT(alloc_.maxpercore);
    static const size_t hugepgsize = GetHugepageSize();

    regionctx &pc = *alloc_.regions[cpu];
    pc.lock.lock();
    if (likely(pc.arenas[arena])) {
        void *ret = pc.arenas[arena];
        pc.arenas[arena] = nullptr;
        pc.lock.unlock();
        return ret;
    }

    void * const mypx = AllocateUnmanagedWithLock(pc, 1);
    return initialize_page(mypx, hugepgsize, (arena + 1) * AllocAlignment);
}

void* SiloRuntime::AllocateUnmanaged(size_t cpu, size_t nhugepgs) {
    regionctx &pc = *alloc_.regions[cpu];
    pc.lock.lock();
    return AllocateUnmanagedWithLock(pc, nhugepgs);
}

void* SiloRuntime::AllocateUnmanagedWithLock(regionctx &pc, size_t nhugepgs) {
    static const size_t hugepgsize = GetHugepageSize();

    void * const mypx = pc.region_begin;

    if (reinterpret_cast<uintptr_t>(mypx) % hugepgsize)
        ALWAYS_ASSERT(false);

    void * const mynewpx =
        reinterpret_cast<char *>(mypx) + nhugepgs * hugepgsize;

    if (unlikely(mynewpx > pc.region_end)) {
        std::cerr << "SiloRuntime[" << runtime_id_
                  << "]::AllocateUnmanagedWithLock: OOM" << std::endl;
        pc.lock.unlock();
        ALWAYS_ASSERT(false);
    }

    const bool needs_mmap = !pc.region_faulted;
    pc.region_begin = mynewpx;
    pc.lock.unlock();

    evt_silo_runtime_region_usage.inc(nhugepgs * hugepgsize);

    if (needs_mmap) {
        void * const x = mmap(mypx, hugepgsize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (unlikely(x == MAP_FAILED)) {
            perror("SiloRuntime::AllocateUnmanagedWithLock: mmap");
            ALWAYS_ASSERT(false);
        }
        INVARIANT(x == mypx);
        const int advice =
            UseMAdvWillNeed() ? MADV_HUGEPAGE | MADV_WILLNEED : MADV_HUGEPAGE;
        if (madvise(x, hugepgsize, advice)) {
            perror("madvise");
            ALWAYS_ASSERT(false);
        }
    }

    return mypx;
}

void SiloRuntime::ReleaseArenas(void **arenas) {
    // cpu -> [(head, tail)]
    std::map<size_t, std::vector<std::pair<void *, void *>>> m;
    for (size_t arena = 0; arena < MAX_ARENAS; arena++) {
        void *p = arenas[arena];
        while (p) {
            void * const pnext = *reinterpret_cast<void **>(p);
            const size_t cpu = PointerToCpu(p);
            auto it = m.find(cpu);
            if (it == m.end()) {
                auto &v = m[cpu];
                v.resize(MAX_ARENAS);
                *reinterpret_cast<void **>(p) = nullptr;
                v[arena].first = v[arena].second = p;
            } else {
                auto &v = it->second;
                if (!v[arena].second) {
                    *reinterpret_cast<void **>(p) = nullptr;
                    v[arena].first = v[arena].second = p;
                } else {
                    *reinterpret_cast<void **>(p) = v[arena].first;
                    v[arena].first = p;
                }
            }
            p = pnext;
        }
    }
    for (auto &p : m) {
        INVARIANT(!p.second.empty());
        regionctx &pc = *alloc_.regions[p.first];
        lock_guard<spinlock> l(pc.lock);
        for (size_t arena = 0; arena < MAX_ARENAS; arena++) {
            INVARIANT(bool(p.second[arena].first) == bool(p.second[arena].second));
            if (!p.second[arena].first)
                continue;
            *reinterpret_cast<void **>(p.second[arena].second) = pc.arenas[arena];
            pc.arenas[arena] = p.second[arena].first;
        }
    }
}

static void numa_hint_memory_placement(void *px, size_t sz, unsigned node) {
    struct bitmask *bm = numa_allocate_nodemask();
    numa_bitmask_setbit(bm, node);
    numa_interleave_memory(px, sz, bm);
    numa_free_nodemask(bm);
}

void SiloRuntime::FaultRegion(size_t cpu) {
    static const size_t hugepgsize = GetHugepageSize();
    ALWAYS_ASSERT(cpu < alloc_.ncpus);
    regionctx &pc = *alloc_.regions[cpu];
    if (pc.region_faulted)
        return;
    lock_guard<std::mutex> l1(pc.fault_lock);
    lock_guard<spinlock> l(pc.lock);
    if (pc.region_faulted)
        return;

    if (reinterpret_cast<uintptr_t>(pc.region_begin) % hugepgsize)
        ALWAYS_ASSERT(false);

    const size_t sz =
        reinterpret_cast<uintptr_t>(pc.region_end) -
        reinterpret_cast<uintptr_t>(pc.region_begin);

    void * const x = mmap(pc.region_begin, sz, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (unlikely(x == MAP_FAILED)) {
        perror("SiloRuntime::FaultRegion: mmap");
        ALWAYS_ASSERT(false);
    }
    ALWAYS_ASSERT(x == pc.region_begin);

    const int advice =
        UseMAdvWillNeed() ? MADV_HUGEPAGE | MADV_WILLNEED : MADV_HUGEPAGE;
    if (madvise(x, sz, advice)) {
        perror("madvise");
        ALWAYS_ASSERT(false);
    }

    numa_hint_memory_placement(
        pc.region_begin,
        (uintptr_t)pc.region_end - (uintptr_t)pc.region_begin,
        numa_node_of_cpu(cpu));

    const size_t nfaults = sz / hugepgsize;
    std::cerr << "runtime" << runtime_id_ << " cpu" << cpu
              << " starting faulting region (" << sz
              << " bytes / " << nfaults << " hugepgs)" << std::endl;
    timer t;
    for (char *px = (char *) pc.region_begin;
         px < (char *) pc.region_end;
         px += CACHELINE_SIZE)
        *px = 0xDE;
    std::cerr << "runtime" << runtime_id_ << " cpu" << cpu
              << " finished faulting region in " << t.lap_ms() << " ms" << std::endl;
    pc.region_faulted = true;
}

void SiloRuntime::DumpStats() {
    std::cerr << "[SiloRuntime " << runtime_id_ << "] ncpus=" << alloc_.ncpus << std::endl;
    for (size_t i = 0; i < alloc_.ncpus; i++) {
        const bool f = alloc_.regions[i]->region_faulted;
        const size_t remaining =
            intptr_t(alloc_.regions[i]->region_end) -
            intptr_t(alloc_.regions[i]->region_begin);
        std::cerr << "[SiloRuntime " << runtime_id_ << "] cpu=" << i
                  << " fully_faulted?=" << f
                  << " remaining=" << remaining << " bytes" << std::endl;
    }
}

// =========================================================================
// Ticker Management
// =========================================================================

ticker& SiloRuntime::get_ticker() {
    if (ticker_) {
        return *ticker_;
    }

    std::lock_guard<std::mutex> lock(ticker_mutex_);
    if (!ticker_) {
        ticker_ = std::make_unique<ticker>();
    }
    return *ticker_;
}

// =========================================================================
// RCU Management
// =========================================================================

rcu& SiloRuntime::get_rcu() {
    if (rcu_) {
        return *rcu_;
    }

    std::lock_guard<std::mutex> lock(rcu_mutex_);
    if (!rcu_) {
        rcu_ = std::make_unique<rcu>(this);
    }
    return *rcu_;
}
