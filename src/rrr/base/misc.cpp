module;

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/sys/process.hpp>
#include <rusty/sys/time.hpp>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

export module rrr.misc;

import std;
import rrr.basetypes;

// @safe - mostly templated helpers (clamp, insert_into_map, erase) +
// Job/OneTimeJob/FrequentJob value classes. The syscall-touching
// functions (`rdtsc`, `time_now_str`, `get_ncpu`, 
// `getline`, the static `make_int` byte-writer) and
// `FrequentJob::Ready` (calls rrr::Time::now(false)) carry per-method
// `// @unsafe` overrides.
export namespace rrr {

// @unsafe - inline `rdtsc` / aarch64 `mrs` asm.
inline uint64_t rdtsc() {
#if defined(__i386__) || defined(__x86_64__)
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (((uint64_t)hi) << 32) | ((uint64_t)lo);
#elif defined(__aarch64__)
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#else
  return 0;
#endif
}

// Authored as inline Rust DSL — multi-parameter fn templates lower
// directly (§7.9); the generated template is byte-equivalent to the
// hand-written one.
#if RUSTYCPP_RUST
fn clamp<T, T1, T2>(v: &T, lower: &T1, upper: &T2) -> T {
    if (*v) < (*lower) {
        return (*lower);
    }
    if (*v) > (*upper) {
        return (*upper);
    }
    return (*v);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=misc.1 version=1 rust_sha256=15cd0d75ad48f830d6171e3faac0b5e8eaa9953aa66df1dd3573e1a2db99c4d5*/
template<typename T, typename T1, typename T2>
T clamp(const T& v, const T1& lower, const T2& upper);

template<typename T, typename T1, typename T2>
T clamp(const T& v, const T1& lower, const T2& upper) {
    if ((v) < (lower)) {
        return (lower);
    }
    if ((v) > (upper)) {
        return (upper);
    }
    return (v);
}
/*RUSTYCPP:GEN-END id=misc.1*/

// YYYY-MM-DD HH:MM:SS.mmm; caller-supplied buffer must be at least 24 bytes.
void time_now_str(char *now);
int get_ncpu();

// NOTE: \n is stripped from input
std::string getline(FILE *fp, char delim = '\n');

template <class K, class V, class Map>
// @unsafe - uses the DEPENDENT TYPE `typename Map::value_type`, which is
// C++ template metaprogramming with no DSL equivalent.
inline void insert_into_map(Map &map, const K &key, const V &value) {
  map.insert(typename Map::value_type(key, value));
}

template <class Container>
typename std::reverse_iterator<typename Container::iterator>
erase(Container &l,
      typename std::reverse_iterator<typename Container::iterator> &rit) {
  typename Container::iterator it = rit.base();
  it--;
  it = l.erase(it);
  return std::reverse_iterator<typename Container::iterator>(it);
}

// `Job` — abstract base trait for unit-of-work scheduling. Concrete
// impls (OneTimeJob, FrequentJob, Alarm) inherit from the emitted
// `class Job` directly; the trait shape is preserved verbatim
// (3 pure virtuals + virtual dtor). Method names stay PascalCase
// (`Ready` / `Work` / `Done`) to match the existing C++ override
// surface — the DSL accepts any valid Rust ident, so the
// non-snake_case names emit unchanged.
//
// First DSL trait in the rrr base layer. Authored as inline Rust;
// the transpiler emits `class Job` at namespace scope because the
// trait is `pub` — same pattern as `PollableBase` in
// `rrr.pollable_proxy` (rusty-cpp main 591aca7 fix).
#if RUSTYCPP_RUST
pub trait Job {
    fn Ready(&mut self) -> bool;
    fn Work(&mut self);
    fn Done(&mut self) -> bool;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=job.0 version=1 rust_sha256=18786ba6577a83b252e1bcef0f636e48705c6028747245d431309a72153fea97*/
class Job;

class Job {
public:
    virtual ~Job() noexcept(false) {}
    virtual bool Ready() = 0;
    virtual void Work() = 0;
    virtual bool Done() = 0;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    Job(Job&&) = delete;
    Job& operator=(Job&&) = delete;
protected:
    Job() = default;
};

template <class U> class JobAdapter;
template <class U> class JobAdapterRef;
template <class U> class JobAdapterRefMut;
/*RUSTYCPP:GEN-END id=job.0*/
// `OneTimeJob` — one-shot Job over a stored callback. Authored as
// inline Rust DSL with #[cpp_inherit] (Job is a DSL interface trait —
// the sanctioned usage): the transpiler emits
// `struct OneTimeJob : public Job` with implicit overrides; call sites
// keep constructing via OneTimeJob::new_ and upcasting Arc<OneTimeJob>
// -> Arc<Job> unchanged.
// Callback alias (the DSL can't parse a Function<..> field type inline).
using OneTimeJobFn = rusty::Function<void()>;

#if RUSTYCPP_RUST
struct OneTimeJob {
    done_: bool,
    ready_: bool,
    func_: OneTimeJobFn,
}

impl OneTimeJob {
    fn new(func: OneTimeJobFn) -> OneTimeJob {
        OneTimeJob { done_: false, ready_: true, func_: func }
    }
}

#[cpp_inherit]
impl Job for OneTimeJob {
    fn Ready(&mut self) -> bool {
        self.ready_
    }

    fn Done(&mut self) -> bool {
        self.done_
    }

    // Runs the one-shot callback exactly once.
    fn Work(&mut self) {
        self.ready_ = false;
        (self.func_)();
        self.done_ = true;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=misc.one_time_job version=1 rust_sha256=6fab16d57545a1a9bbdcd5eb475f330ab39fe0667f2affc062dd6713cc3e2a9e*/
struct OneTimeJob;

struct OneTimeJob : public Job {
    bool done_;
    bool ready_;
    OneTimeJobFn func_;
    OneTimeJob(bool done__init, bool ready__init, OneTimeJobFn func__init) : Job(), done_(std::move(done__init)), ready_(std::move(ready__init)), func_(std::move(func__init)) {}
    OneTimeJob(OneTimeJob&& other) noexcept : Job(), done_(std::move(other.done_)), ready_(std::move(other.ready_)), func_(std::move(other.func_)) {}


    static OneTimeJob new_(OneTimeJobFn func);
    bool Ready();
    bool Done();
    void Work();
};


OneTimeJob OneTimeJob::new_(OneTimeJobFn func) {
    return OneTimeJob(false, true, std::move(func));
}

bool OneTimeJob::Ready() {
    return this->ready_;
}

bool OneTimeJob::Done() {
    return this->done_;
}

void OneTimeJob::Work() {
    this->ready_ = false;
    (this->func_)();
    this->done_ = true;
}
/*RUSTYCPP:GEN-END id=misc.one_time_job*/

} // export namespace rrr

// @safe - impl namespace. Every function below carries its own
// per-method `// @unsafe` because they all touch syscalls or raw
// `char*` buffers; the namespace label is here for future helpers.
namespace rrr {

// @unsafe - writes digits into a caller-supplied raw `char*` buffer.
static void make_int(char* str, int val, int digits) {
    char* p = str + digits;
    for (int i = 0; i < digits; i++) {
        int d = val % 10;
        val /= 10;
        p--;
        *p = '0' + d;
    }
}

// @unsafe - time() + localtime_r syscalls, gettimeofday, and raw
// `char* now` byte-buffer indexing through make_int.
void time_now_str(char* now) {
    time_t seconds_since_epoch = time(nullptr);
    struct tm local_calendar;
    localtime_r(&seconds_since_epoch, &local_calendar);
    make_int(now, local_calendar.tm_year + 1900, 4);
    now[4] = '-';
    make_int(now + 5, local_calendar.tm_mon + 1, 2);
    now[7] = '-';
    make_int(now + 8, local_calendar.tm_mday, 2);
    now[10] = ' ';
    make_int(now + 11, local_calendar.tm_hour, 2);
    now[13] = ':';
    make_int(now + 14, local_calendar.tm_min, 2);
    now[16] = ':';
    make_int(now + 17, local_calendar.tm_sec, 2);
    now[19] = '.';
    timeval tv;
    gettimeofday(&tv, nullptr);
    make_int(now + 20, tv.tv_usec / 1000, 3);
    now[23] = '\0';
}

// Thin wrapper around `rusty::sys::process::sysconf(_SC_NPROCESSORS_ONLN)`.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Same shape as the rrr time
// wrappers (`current_time_us` / `heartbeat_time_us` / etc.) — one-line
// passthroughs into the @safe rusty::sys::* layer.
#if RUSTYCPP_RUST
fn get_ncpu() -> i32 {
    rusty::sys::process::sysconf(_SC_NPROCESSORS_ONLN) as i32
}
#endif
/*RUSTYCPP:GEN-BEGIN id=misc.get_ncpu version=1 rust_sha256=327365961737aad75f0a0355a2f51b97d7bab81213e792a945a6319eac498564*/
int32_t get_ncpu();

int32_t get_ncpu() {
    return static_cast<int32_t>(rusty::sys::process::sysconf(_SC_NPROCESSORS_ONLN));
}
/*RUSTYCPP:GEN-END id=misc.get_ncpu*/


// @unsafe - getdelim allocates the `char* buf` via malloc, hand-managed
// by `free(buf)` at the end. Raw `char*` plumbing throughout.
std::string getline(FILE* fp, char delim) {
    char* buf = nullptr;
    size_t n = 0;
    ssize_t n_read = ::getdelim(&buf, &n, delim, fp);
    if (n_read > 0 && buf[n_read - 1] == delim) {
        n_read--;
    }
    std::string line(buf, n_read);
    free(buf);
    return line;
}

} // namespace rrr
