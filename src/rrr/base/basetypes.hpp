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
//   gettimeofday: [safe, (struct timeval*, struct timezone*) -> int]
//   clock_gettime: [safe, (clockid_t, struct timespec*) -> int]
//   select: [safe, (int, fd_set*, fd_set*, fd_set*, struct timeval*) -> int]
//   pthread_self: [safe, () -> pthread_t]
// }
// Note: struct types like 'timeval' are not functions - they're filtered out in the AST parser

namespace rrr {

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
    void set(i32 v) {
        val_ = v;
    }
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
    v64(i64 v = 0): val_(v) { }
    void set(i64 v) {
        val_ = v;
    }
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
    // @safe - Atomic read of reference count
    int ref_count() const {
        return refcnt_.load(std::memory_order_relaxed);
    }
    // @safe - Atomic increment of reference count
    RefCounted* ref_copy() {
        refcnt_.fetch_add(1, std::memory_order_acq_rel);
        return this;
    }
    // @unsafe - May delete this object
    // SAFETY: Thread-safe via atomic; deletes when refcount reaches 0
    int release() {
        int r = refcnt_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        verify(r >= 0);
        if (r == 0) {
            delete this;
        }
        return r;
    }
};
inline RefCounted::~RefCounted() {}

// @safe
class Counter: public NoCopy {
    std::atomic<i64> next_;
public:
    // @safe
    Counter(i64 start = 0) : next_(start) { }
    // @safe
    i64 peek_next() const {
        return next_.load(std::memory_order_relaxed);
    }
    // @safe
    i64 next(i64 step = 1) {
        return next_.fetch_add(step, std::memory_order_acq_rel);
    }
    // @safe
    void reset(i64 start = 0) {
        next_.store(start, std::memory_order_relaxed);
    }
};

// @safe - Time utilities using system calls marked as safe in external annotations
class Time {
public:
    static const uint64_t RRR_USEC_PER_SEC = 1000000;

    // @safe - Uses clock_gettime which is marked safe in external annotations
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

    // @safe - Uses select which is marked safe in external annotations
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

// @safe - Thread-local random number generator
class Rand: public NoCopy {
    std::mt19937 rand_;
public:
    // @safe - Seeds using current time and thread ID
    Rand();
    // @safe - Returns next random number
    std::mt19937::result_type next() {
        return rand_();
    }
    // @safe - Returns random number in range [lower, upper)
    std::mt19937::result_type next(int lower, int upper) {
        return lower + rand_() % (upper - lower);
    }
    // @safe - Operator() for STL compatibility
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
    // @unsafe - Takes non-owning raw pointer
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
