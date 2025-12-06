/**
 * masstree_context.cc
 *
 * Implementation of MasstreeContext for multi-instance support.
 */
// MasstreeContext functions for thread-local context management
// Most functions are @unsafe due to atomic operations and raw pointers
//
// @external_unsafe_type: std::*
// @external_unsafe: std::*

#include "masstree_context.h"
#include <mutex>

// Thread-local context pointer
thread_local MasstreeContext* tl_masstree_context = nullptr;

// Static ID counter
std::atomic<int> MasstreeContext::s_next_context_id_{0};

// Default global context for backward compatibility
static MasstreeContext* g_default_context = nullptr;
static std::once_flag g_default_context_init;

// @unsafe - uses atomic fetch_add
MasstreeContext::MasstreeContext()
    : context_id_(s_next_context_id_.fetch_add(1))
    , epoch_(1)
    , allthreads_(nullptr) {
}

// @unsafe - uses mutex lock and atomic store
void MasstreeContext::register_threadinfo(threadinfo* ti) {
    std::lock_guard<std::mutex> lock(allthreads_lock_);
    // ti->next_ should already be set by the caller to point to current head
    // This atomically updates the head of the list
    allthreads_.store(ti, std::memory_order_release);
}

// @unsafe - writes to thread-local raw pointer
void MasstreeContext::BindCurrentThread(MasstreeContext* ctx) {
    tl_masstree_context = ctx;
}

// @unsafe - reads thread-local pointer and uses call_once with new
MasstreeContext* MasstreeContext::Current() {
    if (tl_masstree_context) {
        return tl_masstree_context;
    }
    // Lazy-init default context for backward compatibility
    std::call_once(g_default_context_init, []() {
        g_default_context = new MasstreeContext();
    });
    return g_default_context;
}

// @unsafe - allocates with new, caller must manage lifetime
MasstreeContext* MasstreeContext::Create() {
    return new MasstreeContext();
}
