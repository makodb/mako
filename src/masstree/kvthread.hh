/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2016 President and Fellows of Harvard College
 * Copyright (c) 2012-2016 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
// @unsafe - Thread-local state and memory allocation for Masstree
// Provides per-thread memory pools, epoch tracking, and RCU-style reclamation
// SAFETY: Uses malloc/mmap, pthread TLS, and epoch-based memory management

#ifndef KVTHREAD_HH
#define KVTHREAD_HH 1
#include "masstree_context.h"
#include "mtcounters.hh"
#include "compiler.hh"
#include "circular_int.hh"
#include "timestamp.hh"
#include "memdebug.hh"
#include <rusty/ptr.hpp>
#include <atomic>
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdlib.h>

class threadinfo;
class loginfo;

typedef uint64_t mrcu_epoch_type;
typedef int64_t mrcu_signed_epoch_type;

static_assert(std::atomic_ref<mrcu_epoch_type>::is_always_lock_free,
              "Masstree RCU requires lock-free 64-bit participant epochs");
static_assert(alignof(mrcu_epoch_type) >=
                  std::atomic_ref<mrcu_epoch_type>::required_alignment,
              "Masstree RCU participant epochs must satisfy atomic_ref alignment");

// globalepoch is now per-context in MasstreeContext for our code.
// We still declare the extern for backward compatibility with external code
// (like STO benchmarks) that defines and uses their own globalepoch.
extern volatile mrcu_epoch_type globalepoch;

struct limbo_element {
    void* ptr_;
    memtag tag_;
    mrcu_epoch_type epoch_;
};

struct limbo_group {
    enum { capacity = (4076 - sizeof(limbo_group *)) / sizeof(limbo_element) };
    int head_;
    int tail_;
    limbo_element e_[capacity];
    limbo_group *next_;
    limbo_group()
        : head_(0), tail_(0), next_() {
    }
    void push_back(void* ptr, memtag tag, mrcu_epoch_type epoch) {
        assert(tail_ < capacity);
        e_[tail_].ptr_ = ptr;
        e_[tail_].tag_ = tag;
        e_[tail_].epoch_ = epoch;
        ++tail_;
    }
};

template <int N> struct has_threadcounter {
    static bool test(threadcounter ci) {
        return unsigned(ci) < unsigned(N);
    }
};
template <> struct has_threadcounter<0> {
    static bool test(threadcounter) {
        return false;
    }
};

struct mrcu_callback {
    virtual ~mrcu_callback() {
    }
    virtual void operator()(threadinfo& ti) = 0;
};

class threadinfo {
  public:
    enum {
        TI_MAIN, TI_PROCESS, TI_LOG, TI_CHECKPOINT
    };

    // allthreads is now per-context in MasstreeContext

    // @safe - Returns rusty::MutPtr (borrow-checked pointer type)
    rusty::MutPtr<threadinfo> next() const {
        return next_;
    }
    // @safe - Takes rusty::MutPtr parameter
    void set_next(rusty::MutPtr<threadinfo> n) {
        next_ = n;
    }

    // @unsafe { Factory uses placement new }
    static rusty::MutPtr<threadinfo> make(int purpose, int index);
    // XXX destructor

    // thread information
    // @safe - Returns value copy
    int purpose() const {
        return purpose_;
    }
    // @safe - Returns value copy
    int index() const {
        return index_;
    }
    // @safe - Returns rusty::MutPtr (borrow-checked pointer type)
    rusty::MutPtr<MasstreeContext> context() const {
        return context_;
    }
    // @safe - Returns rusty::MutPtr (borrow-checked pointer type)
    rusty::MutPtr<loginfo> logger() const {
        return logger_;
    }
    // @safe - Takes rusty::MutPtr parameter
    void set_logger(rusty::MutPtr<loginfo> logger) {
        assert(!logger_ && logger);
        logger_ = logger;
    }

    // timestamps
    // @unsafe { timestamp() is not borrow-checked }
    kvtimestamp_t operation_timestamp() const {
        return timestamp();
    }
    // @safe - Returns value copy
    kvtimestamp_t update_timestamp() const {
        return ts_;
    }
    // @safe - Returns value copy (ts_ is mutable for internal use)
    kvtimestamp_t update_timestamp(kvtimestamp_t x) const {
        if (circular_int<kvtimestamp_t>::less_equal(ts_, x))
            // x might be a marker timestamp; ensure result is not
            ts_ = (x | 1) + 1;
        return ts_;
    }
    // @unsafe { Accesses raw pointer member of N }
    template <typename N> void observe_phantoms(N* n) {
        if (circular_int<kvtimestamp_t>::less(ts_, n->phantom_epoch_[0]))
            ts_ = n->phantom_epoch_[0];
    }

    // event counters
    // @safe - Modifies owned counter array
    void mark(threadcounter ci) {
        if (has_threadcounter<int(ncounters)>::test(ci))
            ++counters_[ci];
    }
    // @safe - Modifies owned counter array
    void mark(threadcounter ci, int64_t delta) {
        if (has_threadcounter<int(ncounters)>::test(ci))
            counters_[ci] += delta;
    }
    // @unsafe { has_threadcounter::test is not borrow-checked }
    bool has_counter(threadcounter ci) const {
        return has_threadcounter<int(ncounters)>::test(ci);
    }
    // @safe - Returns value copy
    uint64_t counter(threadcounter ci) const {
        return has_threadcounter<int(ncounters)>::test(ci) ? counters_[ci] : 0;
    }

    struct accounting_relax_fence_function {
        rusty::MutPtr<threadinfo> ti_;
        threadcounter ci_;
        accounting_relax_fence_function(rusty::MutPtr<threadinfo> ti, threadcounter ci)
            : ti_(ti), ci_(ci) {
        }
        void operator()() {
            relax_fence();
            ti_->mark(ci_);
        }
    };
    /** @brief Return a function object that calls mark(ci); relax_fence().
     *
     * This function object can be used to count the number of relax_fence()s
     * executed. */
    accounting_relax_fence_function accounting_relax_fence(threadcounter ci) {
        return accounting_relax_fence_function(this, ci);
    }

    struct stable_accounting_relax_fence_function {
        rusty::MutPtr<threadinfo> ti_;
        stable_accounting_relax_fence_function(rusty::MutPtr<threadinfo> ti)
            : ti_(ti) {
        }
        template <typename V>
        void operator()(V v) {
            relax_fence();
            ti_->mark(threadcounter(tc_stable + (v.isleaf() << 1) + v.splitting()));
        }
    };
    /** @brief Return a function object that calls mark(ci); relax_fence().
     *
     * This function object can be used to count the number of relax_fence()s
     * executed. */
    stable_accounting_relax_fence_function stable_fence() {
        return stable_accounting_relax_fence_function(this);
    }

    accounting_relax_fence_function lock_fence(threadcounter ci) {
        return accounting_relax_fence_function(this, ci);
    }

    // memory allocation
    // @unsafe
    // @lifetime: owned
    void* allocate(size_t sz, memtag tag) {
        // @unsafe {
        void* p = malloc(sz + memdebug_size);
        p = memdebug::make(p, sz, tag);
        if (p)
            mark(threadcounter(tc_alloc + (tag > memtag_value)), sz);
        return p;
        // }
    }
    // @unsafe - frees raw heap memory using memdebug headers
    void deallocate(void* p, size_t sz, memtag tag) {
        // in C++ allocators, 'p' must be nonnull
        assert(p);
        p = memdebug::check_free(p, sz, tag);
        free(p);
        mark(threadcounter(tc_alloc + (tag > memtag_value)), -sz);
    }
    // @unsafe - defers free via RCU list; caller promises pointer validity
    void deallocate_rcu(void* p, size_t sz, memtag tag) {
        assert(p);
        memdebug::check_rcu(p, sz, tag);
        record_rcu(p, tag);
        mark(threadcounter(tc_alloc + (tag > memtag_value)), -sz);
    }

    // @unsafe
    // @lifetime: owned
    void* pool_allocate(size_t sz, memtag tag) {
        // @unsafe {
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(nl <= pool_max_nlines);
        if (unlikely(!pool_[nl - 1]))
            refill_pool(nl);
        void* p = pool_[nl - 1];
        if (p) {
            pool_[nl - 1] = *reinterpret_cast<void **>(p);
            p = memdebug::make(p, sz, memtag(tag + nl));
            mark(threadcounter(tc_alloc + (tag > memtag_value)),
                 nl * CACHE_LINE_SIZE);
        }
        return p;
        // }
    }
    // @unsafe - returns raw memory to freelist; assumes caller-provided size/tag
    void pool_deallocate(void* p, size_t sz, memtag tag) {
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(p && nl <= pool_max_nlines);
        p = memdebug::check_free(p, sz, memtag(tag + nl));
        if (use_pool()) {
            *reinterpret_cast<void **>(p) = pool_[nl - 1];
            pool_[nl - 1] = p;
        } else
            free(p);
        mark(threadcounter(tc_alloc + (tag > memtag_value)),
             -nl * CACHE_LINE_SIZE);
    }
    // @unsafe - queues pool frees for later RCU reclamation
    void pool_deallocate_rcu(void* p, size_t sz, memtag tag) {
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(p && nl <= pool_max_nlines);
        memdebug::check_rcu(p, sz, memtag(tag + nl));
        record_rcu(p, memtag(tag + nl));
        mark(threadcounter(tc_alloc + (tag > memtag_value)),
             -nl * CACHE_LINE_SIZE);
    }

    // RCU - Read-Copy-Update memory reclamation
    // @unsafe { Publishes an atomic epoch through a raw context pointer }
    void rcu_start() {
        // Publication must precede every protected Masstree load. Recheck the
        // context epoch so a worker paused between its first snapshot and its
        // publication cannot enter under an epoch a reclaimer has passed.
        mrcu_epoch_type current;
        do {
            current = context_->get_epoch();
            store_gc_epoch(current, std::memory_order_seq_cst);
        } while (current != context_->get_epoch());
    }
    // @unsafe { May call hard_rcu_quiesce which frees memory }
    void rcu_stop() {
        const mrcu_epoch_type current =
            load_gc_epoch(std::memory_order_relaxed);
        if (limbo_epoch_ && (current - limbo_epoch_) > 1)
            hard_rcu_quiesce();
        // Release prevents protected loads from moving past quiescence. A
        // reclaimer that observes zero may therefore ignore this participant.
        store_gc_epoch(0, std::memory_order_release);
    }
    // @unsafe { May call hard_rcu_quiesce which frees memory }
    void rcu_quiesce() {
        rcu_start();
        const mrcu_epoch_type current =
            load_gc_epoch(std::memory_order_relaxed);
        if (limbo_epoch_ && (current - limbo_epoch_) > 2)
            hard_rcu_quiesce();
    }
    typedef ::mrcu_callback mrcu_callback;
    // @unsafe { record_rcu is not borrow-checked }
    void rcu_register(rusty::MutPtr<mrcu_callback> cb) {
        record_rcu(cb, memtag_rcu_callback);
    }

    // thread management
    // @unsafe { Returns mutable reference to pthread_t }
    pthread_t& pthread() {
        return pthreadid_;
    }
    // @unsafe { Returns pthread_t value from non-borrow-checked member }
    pthread_t pthread() const {
        return pthreadid_;
    }

    // @unsafe { Accepts raw pointer for debug output }
    void report_rcu(void* ptr) const;
    // @unsafe { Accepts raw pointer for debug output }
    static void report_rcu_all(void* ptr);

  private:
    mrcu_epoch_type load_gc_epoch(std::memory_order order) const {
        return std::atomic_ref<mrcu_epoch_type>(
                   const_cast<mrcu_epoch_type&>(gc_epoch_))
            .load(order);
    }

    void store_gc_epoch(mrcu_epoch_type epoch, std::memory_order order) {
        std::atomic_ref<mrcu_epoch_type>(gc_epoch_).store(epoch, order);
    }

    union {
        struct {
            mrcu_epoch_type gc_epoch_;
            mrcu_epoch_type limbo_epoch_;
            rusty::MutPtr<loginfo> logger_;

            rusty::MutPtr<threadinfo> next_;
            int purpose_;
            int index_;         // the index of a udp, logging, tcp,
                                // checkpoint or recover thread

            pthread_t pthreadid_;
            rusty::MutPtr<MasstreeContext> context_;  // The context this threadinfo belongs to
        };
        char padding1[CACHE_LINE_SIZE];
    };

    enum { pool_max_nlines = 20 };
    void* pool_[pool_max_nlines];

    limbo_group* limbo_head_;
    limbo_group* limbo_tail_;
    mutable kvtimestamp_t ts_;

    //enum { ncounters = (int) tc_max };
    enum { ncounters = 0 };
    uint64_t counters_[ncounters];

    void refill_pool(int nl);
    void refill_rcu();

    // @unsafe - reclaims memory with manual header checks and freelist rewrites
    void free_rcu(void *p, memtag tag) {
        if ((tag & memtag_pool_mask) == 0) {
            p = memdebug::check_free_after_rcu(p, tag);
            ::free(p);
        } else if (tag == memtag_rcu_callback)
            (*static_cast<mrcu_callback*>(p))(*this);
        else {
            p = memdebug::check_free_after_rcu(p, tag);
            int nl = tag & memtag_pool_mask;
            *reinterpret_cast<void**>(p) = pool_[nl - 1];
            pool_[nl - 1] = p;
        }
    }

    // @unsafe - enqueues raw pointers for later epoch-based free
    void record_rcu(void* ptr, memtag tag) {
        if (limbo_tail_->tail_ == limbo_tail_->capacity)
            refill_rcu();
        uint64_t epoch = context_->get_epoch();
        limbo_tail_->push_back(ptr, tag, epoch);
        if (!limbo_epoch_)
            limbo_epoch_ = epoch;
    }

#if ENABLE_ASSERTIONS
    static int no_pool_value;
    static bool use_pool() {
        return !no_pool_value;
    }
#else
    static bool use_pool() {
        return true;
    }
#endif

    inline threadinfo(int purpose, int index);
    threadinfo(const threadinfo&) = delete;
    ~threadinfo() {}
    threadinfo& operator=(const threadinfo&) = delete;

    void hard_rcu_quiesce();
};

#endif
