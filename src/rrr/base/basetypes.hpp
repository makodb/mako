#pragma once

#include <vector>
#include <queue>
#include <random>
#include <inttypes.h>
#include <atomic>
#include <memory>

#include <time.h>
#include <sys/time.h>

#include "debugging.hpp"

// External safety annotations for system functions used in this module
// @external: {
//   gettimeofday: [unsafe, (struct timeval*, struct timezone*) -> int]
//   clock_gettime: [unsafe, (clockid_t, struct timespec*) -> int]
//   select: [unsafe, (int, fd_set*, fd_set*, fd_set*, struct timeval*) -> int]
//   pthread_self: [unsafe, () -> pthread_t]
//   std::__atomic_base::load: [unsafe]
//   std::__atomic_base::store: [unsafe]
//   std::__atomic_base::fetch_add: [unsafe]
//   std::__atomic_base::fetch_sub: [unsafe]
//   std::mt19937::operator(): [unsafe]
// }
// Note: struct types like 'timeval' are not functions - they're filtered out in the AST parser

namespace rrr {

// @unsafe - Wrapper for atomic store to satisfy borrow checker
template<typename T>
inline void atomic_store_relaxed(std::atomic<T>& atomic_var, T value) {
  atomic_var.store(value, std::memory_order_relaxed);
}

// @unsafe - Wrapper for atomic load to satisfy borrow checker
template<typename T>
inline T atomic_load_relaxed(const std::atomic<T>& atomic_var) {
  return atomic_var.load(std::memory_order_relaxed);
}

// @unsafe - Wrapper for atomic fetch_add to satisfy borrow checker
template<typename T>
inline T atomic_fetch_add_acq_rel(std::atomic<T>& atomic_var, T value) {
  return atomic_var.fetch_add(value, std::memory_order_acq_rel);
}

// @unsafe - Wrapper for atomic fetch_sub to satisfy borrow checker
template<typename T>
inline T atomic_fetch_sub_acq_rel(std::atomic<T>& atomic_var, T value) {
  return atomic_var.fetch_sub(value, std::memory_order_acq_rel);
}

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

// Sparse integer encoding for efficient storage
class SparseInt {
public:
    // @safe - Pure computation, no memory operations
    static size_t buf_size(char byte0);
    // @safe - Pure computation, no memory operations  
    static size_t val_size(i64 val);
    // @unsafe - Uses raw pointer operations for performance
    // SAFETY: Caller must ensure buffer is large enough (at least val_size(val) bytes)
    static size_t dump(i32 val, char* buf);
    // @unsafe - Uses raw pointer operations for performance
    // SAFETY: Caller must ensure buffer is large enough (at least val_size(val) bytes)
    static size_t dump(i64 val, char* buf);
    // @unsafe - Reads from raw pointer
    // SAFETY: Caller must ensure buffer contains valid SparseInt encoding
    static i32 load_i32(const char* buf);
    // @unsafe - Reads from raw pointer
    // SAFETY: Caller must ensure buffer contains valid SparseInt encoding
    static i64 load_i64(const char* buf);
};

// @safe
class v32 {
    i32 val_;
public:
    v32(i32 v = 0): val_(v) { }
    // @safe - Simple assignment
    void set(i32 v) {
        val_ = v;
    }
    // @safe - Simple const getter
    i32 get() const {
        return val_;
    }
    size_t val_size() const {
        return SparseInt::val_size(val_);
    }
};

// @safe
class v64 {
    i64 val_;
public:
    // @safe - Simple value initialization
    v64(i64 v = 0): val_(v) { }
    // @safe - Simple assignment
    void set(i64 v) {
        val_ = v;
    }
    // @safe - Simple const getter
    i64 get() const {
        return val_;
    }
    size_t val_size() const {
        return SparseInt::val_size(val_);
    }
};

// @safe
class NoCopy {
protected:
    NoCopy() = default;
    virtual ~NoCopy() = 0;
public:
    // Delete copy constructor and copy assignment operator
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
    
    // Also delete move operations to prevent any form of copying/moving
    NoCopy(NoCopy&&) = delete;
    NoCopy& operator=(NoCopy&&) = delete;
};
inline NoCopy::~NoCopy() {}

/**
 * Note: All sub class of RefCounted *MUST* have protected destructor!
 * This prevents accidentally deleting the object.
 * You are only allowed to cleanup with release() call.
 * This is thread safe.
 * 
 * SAFETY: Uses atomic reference counting for thread-safe memory management.
 * The protected destructor pattern ensures controlled deallocation.
 */
// @safe - Thread-safe reference counting with atomics
class RefCounted: public NoCopy {
    std::atomic<int> refcnt_;
protected:
    virtual ~RefCounted() = 0;
public:
    RefCounted(): refcnt_(1) {}
    // SAFETY: Atomic read of reference count, thread-safe
    // @unsafe
    int ref_count() const {
        return atomic_load_relaxed(refcnt_);
    }
    // SAFETY: Atomic increment of reference count, thread-safe
    // @unsafe
    RefCounted* ref_copy() {
        atomic_fetch_add_acq_rel(refcnt_, 1);
        return this;
    }
    // SAFETY: Thread-safe via atomic; deletes when refcount reaches 0
    // @unsafe
    int release() {
        int r = atomic_fetch_sub_acq_rel(refcnt_, 1) - 1;
        verify(r >= 0);
        if (r == 0) {
            delete this;
        }
        return r;
    }
};
inline RefCounted::~RefCounted() {}

// @unsafe - All methods call external unsafe atomic operations
// SAFETY: Thread-safe atomic counter
class Counter: public NoCopy {
    std::atomic<i64> next_;
public:
    // @safe - Constructor doesn't call external functions
    Counter(i64 start = 0) : next_(start) { }
    // SAFETY: Atomic read, thread-safe
    // @unsafe
    i64 peek_next() const {
        return atomic_load_relaxed(next_);
    }
    // SAFETY: Atomic increment, thread-safe
    // @unsafe
    i64 next(i64 step = 1) {
        return atomic_fetch_add_acq_rel(next_, step);
    }
    // SAFETY: Atomic write, thread-safe
    // @unsafe
    void reset(i64 start = 0) {
        atomic_store_relaxed(next_, start);
    }
};

// @unsafe - Time utilities using external unsafe system calls
// SAFETY: clock_gettime and select are POSIX-compliant system calls
class Time {
public:
    static const uint64_t RRR_USEC_PER_SEC = 1000000;

    // SAFETY: Properly initializes timespec struct, POSIX-compliant
    // @unsafe
    static uint64_t now(bool accurate = false) {
      struct timespec spec;
#ifdef __APPLE__
      clock_gettime(CLOCK_REALTIME, &spec );
#else
      if (accurate) {
        clock_gettime(CLOCK_MONOTONIC, &spec);
      } else {
        clock_gettime(CLOCK_REALTIME_COARSE, &spec);
      }
#endif
      return spec.tv_sec * RRR_USEC_PER_SEC + spec.tv_nsec/1000;
    }

    // SAFETY: Properly initializes timeval struct, POSIX-compliant
    // @unsafe
    static void sleep(uint64_t t) {
        struct timeval tv;
        tv.tv_usec = t % RRR_USEC_PER_SEC;
        tv.tv_sec = t / RRR_USEC_PER_SEC;
        select(0, NULL, NULL, NULL, &tv);
    }
};

// @safe
class Timer {
public:
    // @safe
    Timer();
    // @safe
    void start();
    // @safe
    void stop();
    // @safe
    void reset();
    // @safe
    double elapsed() const;
private:
    struct timeval begin_;
    struct timeval end_;
};

// @unsafe - Thread-local random number generator calling external unsafe functions
// SAFETY: Uses std::mt19937 which is thread-safe per instance
class Rand: public NoCopy {
    std::mt19937 rand_;
public:
    // @safe - Constructor seeds the generator
    Rand();
    // SAFETY: Thread-safe, each instance is independent
    // @unsafe
    std::mt19937::result_type next() {
        return rand_();
    }
    // SAFETY: Thread-safe, each instance is independent
    // @unsafe
    std::mt19937::result_type next(int lower, int upper) {
        return lower + rand_() % (upper - lower);
    }
    // SAFETY: Thread-safe, each instance is independent
    // @unsafe
    std::mt19937::result_type operator() () {
        return rand_();
    }
};

// @safe
template<class T>
class Enumerator {
public:
    virtual ~Enumerator() {}
    virtual void reset() {
        verify(0);
    }
    virtual bool has_next() = 0;
    operator bool() {
        return this->has_next();
    }
    virtual T next() = 0;
    T operator() () {
        return this->next();
    }
};

// keep min-ordering
// Note: This class stores raw pointers to Enumerator objects
// The caller must ensure these pointers remain valid for the lifetime of MergedEnumerator
template<class T, class Compare = std::greater<T>>
class MergedEnumerator: public Enumerator<T> {
    struct merge_helper {
        T data;
        Enumerator<T>* src;  // Non-owning pointer - lifetime managed externally

        merge_helper(const T& d, Enumerator<T>* s): data(d), src(s) {}

        bool operator < (const merge_helper& other) const {
            return Compare()(data, other.data);
        }
    };

    std::priority_queue<merge_helper, std::vector<merge_helper>> q_;

public:
    // @unsafe
    void add_source(Enumerator<T>* src) {
        if (src && src->has_next()) {
            q_.push(merge_helper(src->next(), src));
        }
    }
    //TODO
    void reset() override {
    }
    bool has_next() override {
        return !q_.empty();
    }
    T next() override {
        verify(!q_.empty());
        const merge_helper& mh = q_.top();
        T ret = mh.data;
        Enumerator<T>* src = mh.src;
        q_.pop();
        if (src->has_next()) {
            q_.push(merge_helper(src->next(), src));
        }
        return ret;
    }
};

} // namespace base
