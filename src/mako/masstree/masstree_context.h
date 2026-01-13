/**
 * masstree_context.h
 *
 * Per-instance context for Masstree to support multiple independent
 * Masstree instances in a single process.
 *
 * Holds per-instance state:
 * - epoch counter (replaces globalepoch)
 * - thread registry (replaces threadinfo::allthreads)
 */

#ifndef MASSTREE_CONTEXT_H
#define MASSTREE_CONTEXT_H

#include <atomic>
#include <mutex>
#include <cstdint>

class threadinfo;

typedef uint64_t mrcu_epoch_type;

/**
 * MasstreeContext - Per-instance context for Masstree
 *
 * Each context maintains its own:
 * - Epoch counter for RCU memory reclamation
 * - List of registered threadinfo objects
 *
 * Thread binding:
 * - Call BindCurrentThread(ctx) to associate current thread with a context
 * - Call Current() to get the context for the current thread
 * - If no context is bound, returns a default global context (backward compat)
 *
 * RustyCpp Safety Annotations:
 * - @safe: Pure getters/setters with atomic operations only
 * - @unsafe: Functions returning raw pointers or accepting raw pointer parameters
 */
class MasstreeContext {
public:
    // @safe - Pure initialization
    MasstreeContext();
    ~MasstreeContext() = default;

    // Non-copyable, non-movable
    MasstreeContext(const MasstreeContext&) = delete;
    MasstreeContext& operator=(const MasstreeContext&) = delete;

    // Epoch management
    // @safe - Returns copy of atomic value
    mrcu_epoch_type get_epoch() const {
        return epoch_.load(std::memory_order_acquire);
    }

    // @safe - Atomic store, no pointer operations
    void set_epoch(mrcu_epoch_type e) {
        epoch_.store(e, std::memory_order_release);
    }

    // @safe - Atomic read-modify-write, no pointer operations
    void increment_epoch(mrcu_epoch_type delta = 2) {
        epoch_.fetch_add(delta, std::memory_order_acq_rel);
    }

    // @unsafe { Returns volatile reference for legacy code patterns, bypasses safety }
    volatile mrcu_epoch_type& epoch_ref() {
        return reinterpret_cast<volatile mrcu_epoch_type&>(epoch_);
    }

    // Thread registry
    // @unsafe { Returns raw pointer to threadinfo }
    threadinfo* get_allthreads() const {
        return allthreads_.load(std::memory_order_acquire);
    }

    // @unsafe { Accepts raw pointer parameter }
    void register_threadinfo(threadinfo* ti);

    // @safe - Returns copy of int value
    int id() const { return context_id_; }

    // @unsafe { Accepts raw pointer parameter }
    static void BindCurrentThread(MasstreeContext* ctx);

    // @unsafe { Returns raw pointer }
    static MasstreeContext* Current();

    // @unsafe { Uses 'new' operator, returns raw pointer }
    static MasstreeContext* Create();

private:
    int context_id_;
    std::atomic<mrcu_epoch_type> epoch_{1};
    std::atomic<threadinfo*> allthreads_{nullptr};
    std::mutex allthreads_lock_;

    static std::atomic<int> s_next_context_id_;
};

// Thread-local context pointer
extern thread_local MasstreeContext* tl_masstree_context;

#endif // MASSTREE_CONTEXT_H
