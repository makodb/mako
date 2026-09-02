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

#include <cstdint>
#include <rusty/mutex.hpp>
#include <rusty/ptr.hpp>
#include <rusty/sync/atomic.hpp>

class threadinfo;

typedef uint64_t mrcu_epoch_type;

/**
 * MasstreeContext - Per-instance context for Masstree
 *
 * Each context maintains its own:
 * - Epoch counter for RCU memory reclamation
 * - List of registered threadinfo objects (mutex-protected head)
 *
 * Thread binding:
 * - Call BindCurrentThread(ctx) to associate current thread with a context
 * - Call Current() to get the context for the current thread
 * - If no context is bound, returns a default global context (backward compat)
 *
 * RustyCpp Safety Annotations:
 * - @safe: pure getters that don't reach into std:: facilities directly
 * - @unsafe: paths that still call the C++ `new` operator or unwrap a
 *   rusty::Result (the LockResult returned by rusty::Mutex)
 *
 * Migration notes (vs the prior version):
 * - `std::mutex allthreads_lock_` plus `std::atomic<MutPtr<threadinfo>>
 *   allthreads_` collapsed into one `rusty::Mutex<MutPtr<threadinfo>>`.
 *   Readers now take the lock briefly; both register_threadinfo and
 *   get_allthreads are called outside the hot insert/lookup path, so
 *   the cost is negligible vs the gain of a single sync primitive.
 * - `std::once_flag` + `std::call_once` migrated to `rusty::Once`.
 * - `std::atomic<mrcu_epoch_type>` and `std::atomic<int>` migrated to
 *   `rusty::sync::atomic::Atomic<T>`.
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
    // @safe - Rusty atomic load wrapper
    mrcu_epoch_type get_epoch() const {
        return epoch_.load(rusty::sync::atomic::Ordering::SeqCst);
    }

    // @safe - Rusty atomic store wrapper
    void set_epoch(mrcu_epoch_type e) {
        epoch_.store(e, rusty::sync::atomic::Ordering::SeqCst);
    }

    // @safe - Rusty atomic fetch_add wrapper
    void increment_epoch(mrcu_epoch_type delta = 2) {
        epoch_.fetch_add(delta, rusty::sync::atomic::Ordering::SeqCst);
    }

    // @unsafe { Returns volatile reference for legacy code patterns, bypasses safety }
    volatile mrcu_epoch_type& epoch_ref() {
        return reinterpret_cast<volatile mrcu_epoch_type&>(epoch_);
    }

    // Thread registry
    // @unsafe { rusty::Mutex::lock returns LockResult; unwrap is @unsafe }
    rusty::MutPtr<threadinfo> get_allthreads() const;

    // @unsafe { rusty::Mutex::lock returns LockResult; unwrap is @unsafe }
    void register_threadinfo(rusty::MutPtr<threadinfo> ti);

    // @safe - Returns copy of int value
    int id() const { return context_id_; }

    // @safe - Assigns thread-local pointer
    static void BindCurrentThread(rusty::MutPtr<MasstreeContext> ctx);

    // @unsafe { rusty::Once::call_once is not borrow-checked }
    static rusty::MutPtr<MasstreeContext> Current();

    // @unsafe { Uses 'new' operator }
    static rusty::MutPtr<MasstreeContext> Create();

private:
    int context_id_;
    rusty::sync::atomic::Atomic<mrcu_epoch_type> epoch_{1};
    // Head of the registered-thread linked list, protected by the
    // mutex. Folding the head pointer into the lock removes the
    // earlier "lock-free read via atomic, serialize writers via
    // separate mutex" split that's easy to get wrong.
    mutable rusty::Mutex<rusty::MutPtr<threadinfo>> allthreads_{nullptr};

    static rusty::sync::atomic::Atomic<int> s_next_context_id_;
};

// Thread-local context pointer
extern thread_local rusty::MutPtr<MasstreeContext> tl_masstree_context;

#endif // MASSTREE_CONTEXT_H
