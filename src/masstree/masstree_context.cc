/**
 * masstree_context.cc
 *
 * Implementation of MasstreeContext for multi-instance support.
 *
 * RustyCpp Safety Notes:
 * - Uses rusty::MutPtr<T> for borrow-checked pointer semantics
 * - Uses rusty::Mutex<T> + rusty::Once instead of std::mutex /
 *   std::lock_guard / std::call_once.
 * - Uses rusty::sync::atomic::Atomic for the epoch counter and the
 *   context-ID counter.
 * - Remaining @unsafe blocks gate the C++ `new` operator and the
 *   LockResult::unwrap() call (Result types' unwrap is @unsafe in
 *   rusty-cpp because they can panic).
 */

#include "masstree_context.h"
#include "kvthread.hh"

#include <rusty/once.hpp>

import std;

// @safe - Uses rusty::MutPtr for borrow-checked semantics
thread_local rusty::MutPtr<MasstreeContext> tl_masstree_context = nullptr;

// Static ID counter (thread-safe atomic, hot only at Create() time).
rusty::sync::atomic::Atomic<int> MasstreeContext::s_next_context_id_{0};

// Global default context, lazily initialized via rusty::Once.
static rusty::MutPtr<MasstreeContext> g_default_context = nullptr;
static rusty::Once g_default_context_init;

// @safe - Uses Rusty atomic fetch_add wrapper
MasstreeContext::MasstreeContext()
    : context_id_(s_next_context_id_.fetch_add(1, rusty::sync::atomic::Ordering::AcqRel))
    , epoch_(1)
    , allthreads_(nullptr) {
}

// @unsafe { rusty::Mutex::lock returns LockResult; unwrap is @unsafe }
rusty::MutPtr<threadinfo> MasstreeContext::get_allthreads() const {
    // @unsafe { LockResult::unwrap can panic in Rust semantics }
    auto guard = allthreads_.lock().unwrap();
    return *guard;
}

// @unsafe { rusty::Mutex::lock returns LockResult; unwrap is @unsafe }
void MasstreeContext::register_threadinfo(rusty::MutPtr<threadinfo> ti) {
    // @unsafe { LockResult::unwrap can panic in Rust semantics }
    auto guard = allthreads_.lock().unwrap();
    // Wire the new threadinfo's next pointer to the current head and
    // promote it to head — all under the lock, no torn intermediate
    // state visible to subsequent registrants.
    ti->set_next(*guard);
    *guard = ti;
}

// @safe - Assigns thread-local pointer
void MasstreeContext::BindCurrentThread(rusty::MutPtr<MasstreeContext> ctx) {
    tl_masstree_context = ctx;
}

// @unsafe { rusty::Once::call_once, 'new' operator are not borrow-checked }
rusty::MutPtr<MasstreeContext> MasstreeContext::Current() {
    if (tl_masstree_context) {
        return tl_masstree_context;
    }
    g_default_context_init.call_once([]() {
        g_default_context = new MasstreeContext();
    });
    return g_default_context;
}

// @unsafe { Uses 'new' operator }
rusty::MutPtr<MasstreeContext> MasstreeContext::Create() {
    return new MasstreeContext();
}
