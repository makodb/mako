/**
 * masstree_context.cc
 *
 * Implementation of MasstreeContext for multi-instance support.
 *
 * RustyCpp Safety Notes:
 * - This file contains unsafe functions due to raw pointer operations
 * - Future migration: Replace raw pointers with rusty::Ptr<T>/MutPtr<T>
 */

#include "masstree_context.h"
#include "kvthread.hh"
#include <mutex>

// @unsafe { Global mutable state via raw pointer }
thread_local MasstreeContext* tl_masstree_context = nullptr;

// Static ID counter (thread-safe atomic)
std::atomic<int> MasstreeContext::s_next_context_id_{0};

// @unsafe { Global mutable state via raw pointer }
static MasstreeContext* g_default_context = nullptr;
static std::once_flag g_default_context_init;

// @safe - Pure initialization, no pointer operations
MasstreeContext::MasstreeContext()
    : context_id_(s_next_context_id_.fetch_add(1))
    , epoch_(1)
    , allthreads_(nullptr) {
}

// @unsafe { Accepts raw pointer, modifies linked list via raw pointers }
void MasstreeContext::register_threadinfo(threadinfo* ti) {
    std::lock_guard<std::mutex> lock(allthreads_lock_);
    // Set next_ inside the lock to avoid race condition where multiple threads
    // read the same head value before any of them register
    ti->set_next(allthreads_.load(std::memory_order_relaxed));  // @unsafe
    allthreads_.store(ti, std::memory_order_release);
}

// @unsafe { Modifies global thread-local raw pointer }
void MasstreeContext::BindCurrentThread(MasstreeContext* ctx) {
    tl_masstree_context = ctx;  // @unsafe
}

// @unsafe { Returns raw pointer, lazy-initializes global state }
MasstreeContext* MasstreeContext::Current() {
    if (tl_masstree_context) {
        return tl_masstree_context;  // @unsafe
    }
    // @unsafe { Lazy-init default context for backward compatibility }
    std::call_once(g_default_context_init, []() {
        g_default_context = new MasstreeContext();  // @unsafe - raw new
    });
    return g_default_context;  // @unsafe
}

// @unsafe { Uses 'new' operator, returns raw pointer }
MasstreeContext* MasstreeContext::Create() {
    return new MasstreeContext();  // @unsafe - raw new
}
