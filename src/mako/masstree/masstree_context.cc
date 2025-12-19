/**
 * masstree_context.cc
 *
 * Implementation of MasstreeContext for multi-instance support.
 */

#include "masstree_context.h"
#include <mutex>

// Thread-local context pointer
thread_local MasstreeContext* tl_masstree_context = nullptr;

// Static ID counter
std::atomic<int> MasstreeContext::s_next_context_id_{0};

// Default global context for backward compatibility
static MasstreeContext* g_default_context = nullptr;
static std::once_flag g_default_context_init;

MasstreeContext::MasstreeContext()
    : context_id_(s_next_context_id_.fetch_add(1))
    , epoch_(1)
    , allthreads_(nullptr) {
}

void MasstreeContext::register_threadinfo(threadinfo* ti) {
    std::lock_guard<std::mutex> lock(allthreads_lock_);
    // ti->next_ should already be set by the caller to point to current head
    // This atomically updates the head of the list
    allthreads_.store(ti, std::memory_order_release);
}

void MasstreeContext::BindCurrentThread(MasstreeContext* ctx) {
    tl_masstree_context = ctx;
}

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

MasstreeContext* MasstreeContext::Create() {
    return new MasstreeContext();
}
