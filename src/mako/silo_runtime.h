/**
 * silo_runtime.h
 *
 * Per-site runtime context for Silo/Mako.
 * Allows multiple independent sites to run in a single process.
 *
 * Each SiloRuntime provides COMPLETE ISOLATION:
 * - An isolated MasstreeContext for the site's Masstree instances
 *   (independent epoch, thread registry, and B-tree state)
 * - Per-site core ID allocation (each site has its own core ID space)
 * - Per-site memory allocator (each site has its own mmap'd region)
 * - Per-site ticker (epoch advancement thread)
 * - Per-site RCU system (deferred reclamation)
 *
 * This design provides complete memory isolation between shards.
 */
// @safe
// Uses RustyCpp smart pointers for memory safety

#ifndef SILO_RUNTIME_H
#define SILO_RUNTIME_H

#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <memory>
#include <rusty/rusty.hpp>
#include "masstree/masstree_context.h"
#include "macros.h"  // For NMAXCORES
#include "spinlock.h"
#include "util.h"

// Forward declarations
class ticker;
class rcu;

/**
 * SiloRuntime - Per-site runtime context
 * @safe - This class follows Rust-like ownership semantics
 *
 * Usage:
 *   // Create a runtime for each site
 *   rusty::Arc<SiloRuntime> site1 = SiloRuntime::Create();
 *   rusty::Arc<SiloRuntime> site2 = SiloRuntime::Create();
 *
 *   // In each site's threads:
 *   SiloRuntime::BindCurrentThread(site1.get());  // or site2
 *
 *   // Access the runtime
 *   SiloRuntime* runtime = SiloRuntime::Current();
 *   MasstreeContext* ctx = runtime->masstree_context();
 */
// @safe
class SiloRuntime {
public:
    // =========================================================================
    // Allocator State (per-runtime memory region)
    // =========================================================================
    static const size_t MAX_ARENAS = 32;
    static const size_t LgAllocAlignment = 4;  // 2^4 = 16 byte alignment
    static const size_t AllocAlignment = 1 << LgAllocAlignment;

    struct regionctx {
        regionctx()
            : region_begin(nullptr),
              region_end(nullptr),
              region_faulted(false) {
            NDB_MEMSET(arenas, 0, sizeof(arenas));
        }
        regionctx(const regionctx &) = delete;
        regionctx(regionctx &&) = delete;
        regionctx &operator=(const regionctx &) = delete;

        void *region_begin;
        void *region_end;
        bool region_faulted;
        spinlock lock;
        std::mutex fault_lock;
        void *arenas[MAX_ARENAS];
    };

    struct AllocatorState {
        void* memstart = nullptr;
        void* memend = nullptr;
        size_t ncpus = 0;
        size_t maxpercore = 0;
        bool initialized = false;
        spinlock init_lock;
        // Using unique_ptr since regionctx is non-copyable/non-movable
        std::vector<std::unique_ptr<regionctx>> regions;
    };

    // @safe
    // @lifetime: owned
    // Factory - create a new runtime for a site
    // Returns Arc for thread-safe shared ownership
    static rusty::Arc<SiloRuntime> Create();

    // @safe
    // Thread binding - associate current thread with a runtime
    // @param runtime: borrowed reference, caller retains ownership
    static void BindCurrentThread(SiloRuntime* runtime);

    // @safe
    // @lifetime: &'static
    // Get the runtime for the current thread
    // Returns borrowed pointer to thread-local runtime
    // Returns the default global runtime if none bound
    static SiloRuntime* Current();

    // @safe
    // @lifetime: &'static
    // Get the default global runtime (for backward compatibility)
    static SiloRuntime* GlobalDefault();

    // @safe
    // Accessors - const methods for reading state
    int id() const { return runtime_id_; }

    // @safe
    // @lifetime: &'self
    // Get the MasstreeContext for this runtime (mutable access)
    MasstreeContext* masstree_context() { return masstree_ctx_.get(); }

    // @safe
    // @lifetime: &'self
    // Get the MasstreeContext for this runtime (const access)
    const MasstreeContext* masstree_context() const { return masstree_ctx_.get(); }

    // =========================================================================
    // Core ID Management (per-runtime core ID space)
    // =========================================================================

    // Maximum cores per runtime
    static const unsigned NMaxCores = NMAXCORES;

    // @safe
    // Allocate a new core ID from this runtime's ID space
    // Returns the allocated core ID
    // Thread-safe: uses atomic fetch_add
    unsigned allocate_core_id();

    // @unsafe: uses atomic operations
    // Get the current core count for this runtime
    unsigned core_count() const {
        return core_count_.load(std::memory_order_acquire);
    }

    // @safe
    // Allocate a contiguous block of core IDs with alignment
    // Returns the starting core ID, or -1 if allocation would exceed max
    int allocate_contiguous_aligned_block(unsigned n, unsigned alignment);

    // =========================================================================
    // Allocator Management (per-runtime memory)
    // =========================================================================

    // Initialize the allocator for this runtime
    // ncpus: number of per-cpu regions to create
    // maxpercore: max bytes per cpu region
    void InitializeAllocator(size_t ncpus, size_t maxpercore);

    // Get allocator state
    AllocatorState& allocator_state() { return alloc_; }
    const AllocatorState& allocator_state() const { return alloc_; }

    // Allocate from this runtime's memory pool
    void* AllocateArenas(size_t cpu, size_t arena);
    void* AllocateUnmanaged(size_t cpu, size_t nhugepgs);
    void ReleaseArenas(void **arenas);
    void FaultRegion(size_t cpu);
    void DumpStats();

    // Check if pointer is managed by this runtime's allocator
    bool ManagesPointer(const void *p) const {
        return p >= alloc_.memstart && p < alloc_.memend;
    }

    // Get CPU index for a pointer (assumes ManagesPointer is true)
    size_t PointerToCpu(const void *p) const;

    // =========================================================================
    // Ticker Management (per-runtime epoch advancement)
    // =========================================================================

    // Get the ticker for this runtime (lazily created)
    ticker& get_ticker();

    // =========================================================================
    // RCU Management (per-runtime deferred reclamation)
    // =========================================================================

    // Get the RCU instance for this runtime (lazily created)
    rcu& get_rcu();

    // @unsafe
    // Convenience: bind this runtime to current thread and also bind its MasstreeContext
    // @unsafe because MasstreeContext::BindCurrentThread is not annotated
    void BindToCurrentThread() {
        SiloRuntime::BindCurrentThread(this);
        MasstreeContext::BindCurrentThread(masstree_ctx_.get());
    }

    // Non-copyable, non-movable (prevent accidental ownership transfer)
    SiloRuntime(const SiloRuntime&) = delete;
    SiloRuntime& operator=(const SiloRuntime&) = delete;
    SiloRuntime(SiloRuntime&&) = delete;
    SiloRuntime& operator=(SiloRuntime&&) = delete;

    // @safe
    ~SiloRuntime();

    // Constructor - prefer Create() factory for Arc-wrapped instances
    // @safe
    SiloRuntime();

private:
    // Helper for allocator
    void* AllocateUnmanagedWithLock(regionctx &pc, size_t nhugepgs);

    // Instance ID for debugging/identification
    int runtime_id_;

    // @lifetime: owned
    // Owned MasstreeContext - each runtime has exactly one
    rusty::Box<MasstreeContext> masstree_ctx_;

    // Per-runtime core ID counter
    // Each runtime has its own core ID space starting at 0
    std::atomic<unsigned> core_count_{0};

    // Per-runtime allocator state
    AllocatorState alloc_;

    // Per-runtime ticker (lazily created)
    std::unique_ptr<ticker> ticker_;
    std::mutex ticker_mutex_;

    // Per-runtime RCU (lazily created)
    std::unique_ptr<rcu> rcu_;
    std::mutex rcu_mutex_;

    // Static members for global state management
    // Atomic counter for generating unique runtime IDs
    static std::atomic<int> s_next_runtime_id_;

    // @lifetime: 'static
    // Global default runtime (lazily initialized, lives for program duration)
    // Using rusty::Arc for thread-safe access
    static rusty::Arc<SiloRuntime> s_global_default_;

    // Mutex for thread-safe lazy initialization of global default
    static std::mutex s_global_mutex_;
};

// Thread-local runtime pointer
// @unsafe - Thread-local storage requires careful lifetime management
// The pointed-to SiloRuntime must outlive all threads using it
extern thread_local SiloRuntime* tl_silo_runtime;

#endif // SILO_RUNTIME_H
