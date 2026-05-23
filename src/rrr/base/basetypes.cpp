module;

#include <rusty/rusty.hpp>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

export module rrr.basetypes;

import std;

// @safe - POD/value-type helpers + small classes (SparseInt, v32/v64,
// NoCopy, Counter, Time, Timer, Rand, Enumerator, MergedEnumerator).
// Time / Timer time syscalls (clock_gettime, gettimeofday, nanosleep)
// now flow through `rusty::sys::time::*` helpers (each itself @safe
// with an inner @unsafe block). The remaining per-method `// @unsafe`
// overrides cover raw `char*` byte slicing via `reinterpret_cast<char*>`
// and `pthread_self`-based hashing in `Rand`.
export namespace rrr {

template<typename T>
inline void atomic_store_relaxed(std::atomic<T>& atomic_var, T value) {
  atomic_var.store(value, std::memory_order_relaxed);
}

template<typename T>
inline T atomic_load_relaxed(const std::atomic<T>& atomic_var) {
  return atomic_var.load(std::memory_order_relaxed);
}

template<typename T>
inline T atomic_fetch_add_acq_rel(std::atomic<T>& atomic_var, T value) {
  return atomic_var.fetch_add(value, std::memory_order_acq_rel);
}

template<typename T>
inline T atomic_fetch_sub_acq_rel(std::atomic<T>& atomic_var, T value) {
  return atomic_var.fetch_sub(value, std::memory_order_acq_rel);
}

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

class SparseInt {
public:
    static size_t buf_size(char byte0);
    static size_t val_size(i64 val);
    static size_t dump(i32 val, char* buf);
    static size_t dump(i64 val, char* buf);
    static i32 load_i32(const char* buf);
    static i64 load_i64(const char* buf);
};

class v32 {
    i32 val_;
public:
    v32(i32 v = 0): val_(v) { }
    void set(i32 v) { val_ = v; }
    i32 get() const { return val_; }
    size_t val_size() const { return SparseInt::val_size(val_); }
};

class v64 {
    i64 val_;
public:
    v64(i64 v = 0): val_(v) { }
    void set(i64 v) { val_ = v; }
    i64 get() const { return val_; }
    size_t val_size() const { return SparseInt::val_size(val_); }
};

class NoCopy {
protected:
    NoCopy() = default;
    virtual ~NoCopy() = default;
public:
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
    NoCopy(NoCopy&&) = default;
    NoCopy& operator=(NoCopy&&) = default;
};

class Counter: public NoCopy {
    std::atomic<i64> next_;
public:
    Counter(i64 start = 0) : next_(start) { }
    i64 peek_next() const {
        return atomic_load_relaxed(next_);
    }
    i64 next(i64 step = 1) {
        return atomic_fetch_add_acq_rel(next_, step);
    }
    void reset(i64 start = 0) {
        atomic_store_relaxed(next_, start);
    }
};

class Time {
public:
    static const uint64_t RRR_USEC_PER_SEC = 1000000;

    // @safe - delegates to rusty::sys::time::clock_*_us(), each of
    // which wraps its clock_gettime call in an inner @unsafe block.
    static uint64_t now(bool accurate = false) {
#ifdef __APPLE__
        return rusty::sys::time::clock_realtime_us();
#else
        return accurate ? rusty::sys::time::clock_monotonic_us()
                        : rusty::sys::time::clock_realtime_coarse_us();
#endif
    }

    // @safe - delegates to rusty::sys::time::sleep_us, which wraps
    // nanosleep in an inner @unsafe block. (Replaces the historical
    // select(0,NULL,NULL,NULL,&tv) sleep idiom.)
    static void sleep(uint64_t t) {
        rusty::sys::time::sleep_us(t);
    }
};

class Timer {
public:
    Timer();
    void start();
    void stop();
    void reset();
    double elapsed() const;
private:
    struct timeval begin_;
    struct timeval end_;
};

class Rand: public NoCopy {
    std::mt19937 rand_;
public:
    Rand();
    std::mt19937::result_type next() {
        return rand_();
    }
    std::mt19937::result_type next(int lower, int upper) {
        return lower + rand_() % (upper - lower);
    }
    std::mt19937::result_type operator() () {
        return rand_();
    }
};

template<class T>
class Enumerator {
public:
    virtual ~Enumerator() {}
    virtual void reset() {
        std::abort();
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

template<class T, class Compare = std::greater<T>>
class MergedEnumerator: public Enumerator<T> {
    struct merge_helper {
        T data;
        Enumerator<T>* src;
        merge_helper(const T& d, Enumerator<T>* s): data(d), src(s) {}
        bool operator < (const merge_helper& other) const {
            return Compare()(data, other.data);
        }
    };

    rusty::Vec<merge_helper> q_;

public:
    // @unsafe - takes raw `Enumerator<T>*`; calls std::push_heap on raw
    // iterator pair.
    void add_source(Enumerator<T>* src) {
        if (src && src->has_next()) {
            q_.push(merge_helper(src->next(), src));
            std::push_heap(q_.begin(), q_.end());
        }
    }
    void reset() override {
    }
    bool has_next() override {
        return !q_.is_empty();
    }
    // @unsafe - raw `Enumerator<T>*` dereference + std::pop/push_heap on
    // raw iterator pairs.
    T next() override {
        if (q_.is_empty()) std::abort();
        std::pop_heap(q_.begin(), q_.end());
        merge_helper mh = q_.pop();
        T ret = mh.data;
        Enumerator<T>* src = mh.src;
        if (src->has_next()) {
            q_.push(merge_helper(src->next(), src));
            std::push_heap(q_.begin(), q_.end());
        }
        return ret;
    }
};

} // export namespace rrr

// @safe - impl block: buf_size/val_size are pure switch math; the
// dump/load_* methods do `reinterpret_cast<char*>` + raw `char*`
// byte slicing so they carry per-method `// @unsafe`; Timer::* and
// Rand::* hit `gettimeofday` / `pthread_self` and carry per-method
// `// @unsafe`.
namespace rrr {

size_t SparseInt::buf_size(char byte0) {
    if ((byte0 & 0x80) == 0) {
        return 1;
    } else if ((byte0 & 0xC0) == 0x80) {
        return 2;
    } else if ((byte0 & 0xE0) == 0xC0) {
        return 3;
    } else if ((byte0 & 0xF0) == 0xE0) {
        return 4;
    } else if ((byte0 & 0xF8) == 0xF0) {
        return 5;
    } else if ((byte0 & 0xFC) == 0xF8) {
        return 6;
    } else if ((byte0 & 0xFE) == 0xFC) {
        return 7;
    } else if ((byte0 & 0xFF) == 0xFE) {
        return 8;
    } else {
        return 9;
    }
}

size_t SparseInt::val_size(i64 val) {
    if (-64 <= val && val <= 63) {
        return 1;
    } else if (-8192 <= val && val <= 8191) {
        return 2;
    } else if (-1048576 <= val && val <= 1048575) {
        return 3;
    } else if (-134217728 <= val && val <= 134217727) {
        return 4;
    } else if (-17179869184LL <= val && val <= 17179869183LL) {
        return 5;
    } else if (-2199023255552LL <= val && val <= 2199023255551LL) {
        return 6;
    } else if (-281474976710656LL <= val && val <= 281474976710655LL) {
        return 7;
    } else if (-36028797018963968LL <= val && val <= 36028797018963967LL) {
        return 8;
    } else {
        return 9;
    }
}

// @unsafe - reinterpret_cast<char*> + raw `char*` byte indexing.
size_t SparseInt::dump(i32 val, char* buf) {
    char* pv = reinterpret_cast<char*>(&val);
    if (-64 <= val && val <= 63) {
        buf[0] = pv[0];
        buf[0] &= 0x7F;
        return 1;
    } else if (-8192 <= val && val <= 8191) {
        buf[0] = pv[1];
        buf[1] = pv[0];
        buf[0] &= 0x3F;
        buf[0] |= 0x80;
        return 2;
    } else if (-1048576 <= val && val <= 1048575) {
        buf[0] = pv[2];
        buf[1] = pv[1];
        buf[2] = pv[0];
        buf[0] &= 0x1F;
        buf[0] |= 0xC0;
        return 3;
    } else if (-134217728 <= val && val <= 134217727) {
        buf[0] = pv[3];
        buf[1] = pv[2];
        buf[2] = pv[1];
        buf[3] = pv[0];
        buf[0] &= 0x0F;
        buf[0] |= 0xE0;
        return 4;
    } else {
        buf[1] = pv[3];
        buf[2] = pv[2];
        buf[3] = pv[1];
        buf[4] = pv[0];
        if (val < 0) {
            buf[0] = 0xF7;
        } else {
            buf[0] = 0xF0;
        }
        return 5;
    }
}

// @unsafe - reinterpret_cast<char*> + raw `char*` byte indexing.
size_t SparseInt::dump(i64 val, char* buf) {
    char* pv = reinterpret_cast<char*>(&val);
    if (-64 <= val && val <= 63) {
        buf[0] = pv[0];
        buf[0] &= 0x7F;
        return 1;
    } else if (-8192 <= val && val <= 8191) {
        buf[0] = pv[1];
        buf[1] = pv[0];
        buf[0] &= 0x3F;
        buf[0] |= 0x80;
        return 2;
    } else if (-1048576 <= val && val <= 1048575) {
        buf[0] = pv[2];
        buf[1] = pv[1];
        buf[2] = pv[0];
        buf[0] &= 0x1F;
        buf[0] |= 0xC0;
        return 3;
    } else if (-134217728 <= val && val <= 134217727) {
        buf[0] = pv[3];
        buf[1] = pv[2];
        buf[2] = pv[1];
        buf[3] = pv[0];
        buf[0] &= 0x0F;
        buf[0] |= 0xE0;
        return 4;
    } else if (-17179869184LL <= val && val <= 17179869183LL) {
        buf[0] = pv[4];
        buf[1] = pv[3];
        buf[2] = pv[2];
        buf[3] = pv[1];
        buf[4] = pv[0];
        buf[0] &= 0x07;
        buf[0] |= 0xF0;
        return 5;
    } else if (-2199023255552LL <= val && val <= 2199023255551LL) {
        buf[0] = pv[5];
        buf[1] = pv[4];
        buf[2] = pv[3];
        buf[3] = pv[2];
        buf[4] = pv[1];
        buf[5] = pv[0];
        buf[0] &= 0x03;
        buf[0] |= 0xF8;
        return 6;
    } else if (-281474976710656LL <= val && val <= 281474976710655LL) {
        buf[0] = pv[6];
        buf[1] = pv[5];
        buf[2] = pv[4];
        buf[3] = pv[3];
        buf[4] = pv[2];
        buf[5] = pv[1];
        buf[6] = pv[0];
        buf[0] &= 0x01;
        buf[0] |= 0xFC;
        return 7;
    } else if (-36028797018963968LL <= val && val <= 36028797018963967LL) {
        buf[1] = pv[7];
        buf[2] = pv[6];
        buf[3] = pv[5];
        buf[4] = pv[4];
        buf[5] = pv[3];
        buf[6] = pv[2];
        buf[7] = pv[1];
        buf[8] = pv[0];
        buf[0] = 0xFE;
        return 8;
    } else {
        buf[1] = pv[7];
        buf[2] = pv[6];
        buf[3] = pv[5];
        buf[4] = pv[4];
        buf[5] = pv[3];
        buf[6] = pv[2];
        buf[7] = pv[1];
        buf[8] = pv[0];
        buf[0] = 0xFF;
        return 9;
    }
}

// @unsafe - reinterpret_cast<char*> + raw `char*` byte indexing.
i32 SparseInt::load_i32(const char* buf) {
    i32 val = 0;
    char* pv = reinterpret_cast<char*>(&val);
    int bsize = SparseInt::buf_size(buf[0]);
    if (bsize < 5) {
        for (int i = 0; i < bsize; i++) {
            pv[i] = buf[bsize - i - 1];
        }
        pv[bsize - 1] &= 0xFF >> bsize;
        if ((pv[bsize - 1] >> (7 - bsize)) & 0x1) {
            pv[bsize - 1] |= 0xFF << (7 - bsize);
            for (int i = bsize; i < 4; i++) {
                pv[i] = 0xFF;
            }
        }
    } else {
        for (int i = 0; i < 4; i++) {
            pv[i] = buf[4 - i];
        }
    }
    return val;
}

// @unsafe - reinterpret_cast<char*> + raw `char*` byte indexing.
i64 SparseInt::load_i64(const char* buf) {
    i64 val = 0;
    char* pv = reinterpret_cast<char*>(&val);
    int bsize = SparseInt::buf_size(buf[0]);
    if (bsize < 8) {
        for (int i = 0; i < bsize; i++) {
            pv[i] = buf[bsize - i - 1];
        }
        pv[bsize - 1] &= 0xFF >> bsize;
        if ((pv[bsize - 1] >> (7 - bsize)) & 0x1) {
            pv[bsize - 1] |= 0xFF << (7 - bsize);
            for (int i = bsize; i < 8; i++) {
                pv[i] = 0xFF;
            }
        }
    } else {
        for (int i = 0; i < 8; i++) {
            pv[i] = buf[8 - i];
        }
    }
    return val;
}

Timer::Timer() : begin_(), end_() {
    reset();
}

// @safe - delegates to rusty::sys::time::gettimeofday_us, which wraps
// gettimeofday(2) in an inner @unsafe block.
void Timer::start() {
    reset();
    const std::uint64_t now = rusty::sys::time::gettimeofday_us();
    begin_.tv_sec  = static_cast<time_t>(now / 1000000);
    begin_.tv_usec = static_cast<suseconds_t>(now % 1000000);
}

// @safe - delegates to rusty::sys::time::gettimeofday_us.
void Timer::stop() {
    const std::uint64_t now = rusty::sys::time::gettimeofday_us();
    end_.tv_sec  = static_cast<time_t>(now / 1000000);
    end_.tv_usec = static_cast<suseconds_t>(now % 1000000);
}

void Timer::reset() {
    begin_.tv_sec = 0;
    begin_.tv_usec = 0;
    end_.tv_sec = 0;
    end_.tv_usec = 0;
}

// @safe - live-elapsed branch delegates to rusty::sys::time::gettimeofday_us.
double Timer::elapsed() const {
    if (begin_.tv_sec == 0 && begin_.tv_usec == 0) std::abort();
    if (end_.tv_sec == 0 && end_.tv_usec == 0) {
        const std::uint64_t now_us = rusty::sys::time::gettimeofday_us();
        const std::uint64_t begin_us =
            static_cast<std::uint64_t>(begin_.tv_sec) * 1000000 + begin_.tv_usec;
        return static_cast<double>(now_us - begin_us) / 1000000.0;
    }
    return end_.tv_sec - begin_.tv_sec + (end_.tv_usec - begin_.tv_usec) / 1000000.0;
}

// @unsafe - pthread_self + reinterpret_cast<uintptr_t>(this). The
// gettimeofday call is itself @safe (rusty::sys::time::gettimeofday_us),
// but the seed mix-in still needs pthread_self + address-of-this which
// aren't analyzable.
Rand::Rand() : rand_() {
    const std::uint64_t now_us = rusty::sys::time::gettimeofday_us();
    const auto thread_hash =
        static_cast<long long>(std::hash<pthread_t>{}(pthread_self()));
    const auto this_hash =
        static_cast<long long>(reinterpret_cast<uintptr_t>(this));
    rand_.seed(static_cast<long long>(now_us) + thread_hash + this_hash);
}

} // namespace rrr
