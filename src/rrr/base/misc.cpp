module;

#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/sys/process.hpp>
#include <rusty/sys/time.hpp>
// Reachability: format_thousands' GEN names rusty::detail::deref_if_pointer_like.
#include <rusty/slice.hpp>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

export module rrr.misc;

import std;
import rrr.basetypes;

// @safe - the clamp template +
// Job/OneTimeJob/FrequentJob value classes. The syscall-touching
// functions (`get_ncpu`, `time_now_str`) and `FrequentJob::Ready`
// (calls rrr::Time::now(false)) carry per-method `// @unsafe`
// overrides.
export namespace rrr {


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


int get_ncpu();


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


// Thousands-grouped 2-decimal formatter (was the strop format_decimal
// double/int overload pair; the int overload had zero callers and Rust
// has no overloading, so one DSL fn under a new name serves the single
// consumer, test-helper's report_qps).
#if RUSTYCPP_RUST
fn format_thousands(val: f64) -> std::string {
    let s: std::string = format!("{:.2}", val);
    let mut dot: usize = 0usize;
    while dot < s.size() {
        if s[dot] == '.' {
            break;
        }
        dot += 1usize;
    }
    let mut out: std::string = format!("");
    let mut i: usize = 0usize;
    while i < dot {
        if (dot - i) % 3usize == 0usize && i != 0usize && s[i - 1usize] != '-' {
            out.push_back(',');
        }
        out.push_back(s[i]);
        i += 1usize;
    }
    out += s.substr(dot);
    if out == "-0.00" {
        return format!("0.00");
    }
    out
}
#endif
/*RUSTYCPP:GEN-BEGIN id=misc.4 version=1 rust_sha256=3635c241e419471cb8d61493e4db5f3155a4276bf8b731505905edcbd18f86d9*/
std::string format_thousands(double val);

std::string format_thousands(double val) {
    const std::string s = std::format("{:.2f}", val);
    size_t dot = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(dot) < s.size()) {
        if (s[dot] == U'.') {
            break;
        }
        dot += static_cast<size_t>(1);
    }
    std::string out = std::format("");
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(dot)) {
        if ((((((rusty::detail::deref_if_pointer_like(dot) - rusty::detail::deref_if_pointer_like(i))) % static_cast<size_t>(3)) == static_cast<size_t>(0)) && (rusty::detail::deref_if_pointer_like(i) != static_cast<size_t>(0))) && (s[rusty::detail::deref_if_pointer_like(i) - static_cast<size_t>(1)] != U'-')) {
            out.push_back(U',');
        }
        out.push_back(s[i]);
        i += static_cast<size_t>(1);
    }
    out += s.substr(std::move(dot));
    if (out == "-0.00") {
        return std::format("0.00");
    }
    return std::move(out);
}
/*RUSTYCPP:GEN-END id=misc.4*/

} // export namespace rrr

// @safe - impl namespace. Every function below carries its own
// per-method `// @unsafe` because they all touch syscalls or raw
// `char*` buffers; the namespace label is here for future helpers.
namespace rrr {

// The `time_now_str` shim and its `extern "C" srpc_time_now_str`
// bridge declaration are gone. The timestamp formatter lives in
// srpc_timing.c (plain C, Goal-0 C demotion) and its ONE consumer --
// base/logging.cpp's `log_time_now` kernel -- now declares and calls
// the C entry point directly. Converting the shim to DSL instead would
// have been a net loss: `*mut i8` lowers to `int8_t*`, which does not
// bind the caller's `char[24]` without adding a reinterpret_cast.

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


} // namespace rrr
