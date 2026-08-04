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

// The cycle-counter read lives in srpc_timing.c now (plain C, Goal-0 C
// demotion — inline asm will never be Rust DSL).
extern "C" uint64_t srpc_rdtsc_raw(void);
inline uint64_t rdtsc() {
  return srpc_rdtsc_raw();
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

#if RUSTYCPP_RUST
struct OneTimeJob {
    done_: bool,
    ready_: bool,
    func_: rusty::Function<dyn FnMut()>,
}

impl OneTimeJob {
    fn new(func: rusty::Function<dyn FnMut()>) -> OneTimeJob {
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
/*RUSTYCPP:GEN-BEGIN id=misc.one_time_job version=1 rust_sha256=811950b81a2fba1d8570507b860d09abc088434e0af080f2bbfffb10e7ed627c*/
struct OneTimeJob;

struct OneTimeJob : public Job {
    bool done_;
    bool ready_;
    rusty::Function<void()> func_;
    OneTimeJob(bool done__init, bool ready__init, rusty::Function<void()> func__init) : Job(), done_(std::move(done__init)), ready_(std::move(ready__init)), func_(std::move(func__init)) {}
    OneTimeJob(OneTimeJob&& other) noexcept : Job(), done_(std::move(other.done_)), ready_(std::move(other.ready_)), func_(std::move(other.func_)) {}


    static OneTimeJob new_(rusty::Function<void()> func);
    bool Ready();
    bool Done();
    void Work();
};


OneTimeJob OneTimeJob::new_(rusty::Function<void()> func) {
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


// Relocated from the deleted rrr.strop module (its only live symbols;
// startswith/endswith/strsplit were dead and went with the module).
// @unsafe - OVERLOADED NAME (double/int pair). Rust has no function
// overloading, so the two cannot coexist as one DSL fn (7.24a); route
// is callsite_rewrite (test-helper + memdb re-export are the only
// consumers).
std::string format_decimal(double val) {
    std::ostringstream o;
    o.precision(2);
    o << std::fixed << val;
    std::string s(o.str());
    std::string str;
    size_t idx = 0;
    while (idx < s.size()) {
        if (s[idx] == '.') {
            break;
        }
        idx++;
    }
    str.reserve(s.size() + 16);
    for (size_t i = 0; i < idx; i++) {
        if ((idx - i) % 3 == 0 && i != 0 && s[i - 1] != '-') {
            str += ',';
        }
        str += s[i];
    }
    str += s.substr(idx);
    if (str == "-0.00") {
        str = "0.00";
    }
    return str;
}

std::string format_decimal(int val) {
    std::ostringstream o;
    o << val;
    std::string s(o.str());
    std::string str;
    str.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        if ((s.size() - i) % 3 == 0 && i != 0 && s[i - 1] != '-') {
            str += ',';
        }
        str += s[i];
    }
    return str;
}

} // export namespace rrr

// @safe - impl namespace. Every function below carries its own
// per-method `// @unsafe` because they all touch syscalls or raw
// `char*` buffers; the namespace label is here for future helpers.
namespace rrr {

// The timestamp formatter (time/localtime_r/gettimeofday + raw digit
// writing, formerly make_int + this body) lives in srpc_timing.c now
// (plain C, Goal-0 C demotion).
extern "C" void srpc_time_now_str(char* now);

// @unsafe - thin shim over the C kernel (raw char* passthrough).
void time_now_str(char* now) {
    srpc_time_now_str(now);
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
