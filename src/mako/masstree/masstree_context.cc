/**
 * masstree_context.cc
 *
 * Implementation of MasstreeContext for multi-instance support.
 *
 * RustyCpp Safety Notes:
 * - Uses rusty::MutPtr<T> for borrow-checked pointer semantics
 * - Remaining @unsafe blocks for 'new' operator allocations
 */

#include "masstree_context.h"
#include "kvthread.hh"
#include <mutex>

// @safe - Uses rusty::MutPtr for borrow-checked semantics
thread_local rusty::MutPtr<MasstreeContext> tl_masstree_context = nullptr;

// Static ID counter (thread-safe atomic)
std::atomic<int> MasstreeContext::s_next_context_id_{0};

// Global default context (rusty::MutPtr for safety)
static rusty::MutPtr<MasstreeContext> g_default_context = nullptr;
static std::once_flag g_default_context_init;

// @safe - Pure initialization, no pointer operations
MasstreeContext::MasstreeContext()
    : context_id_(s_next_context_id_.fetch_add(1))
    , epoch_(1)
    , allthreads_(nullptr) {
}

// @safe - Uses rusty::MutPtr, modifies linked list
void MasstreeContext::register_threadinfo(rusty::MutPtr<threadinfo> ti) {
    std::lock_guard<std::mutex> lock(allthreads_lock_);
    // Set next_ inside the lock to avoid race condition where multiple threads
    // read the same head value before any of them register
    ti->set_next(allthreads_.load(std::memory_order_relaxed));
    allthreads_.store(ti, std::memory_order_release);
}

// @safe - Uses rusty::MutPtr
void MasstreeContext::BindCurrentThread(rusty::MutPtr<MasstreeContext> ctx) {
    tl_masstree_context = ctx;
}

// @safe - Returns rusty::MutPtr, lazy-initializes global state via @unsafe block
rusty::MutPtr<MasstreeContext> MasstreeContext::Current() {
    if (tl_masstree_context) {
        return tl_masstree_context;
    }
    // @unsafe { Uses 'new' operator for lazy-init }
    std::call_once(g_default_context_init, []() {
        g_default_context = new MasstreeContext();
    });
    return g_default_context;
}

// @unsafe { Uses 'new' operator }
rusty::MutPtr<MasstreeContext> MasstreeContext::Create() {
    return new MasstreeContext();
}
