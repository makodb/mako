module;

#include <rusty/array.hpp>
#include <rusty/mutex.hpp>
#include <rusty/once.hpp>
#include <rusty/rusty.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/times.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

export module rrr.cpuinfo;

import std;
import rusty;
import rrr.logging;

// @safe - CPUInfo: process-level cpu/network/memory sampling. The
// ctor, `get_cpu_stat`, `get_network`, and `get_memory` all carry
// per-method `// @unsafe` because they do syscalls (sysinfo, sysconf,
// times, getpid); /proc parsing is DSL over rusty::sys::fs::
// read_to_string + find/substr walking. The `cpu_stat()` factory hands
// out the static instance and inherits namespace @safe.
export namespace rrr {

struct CPUInfo;

// Backing free fns for the DSL `cpu_stat` below. `cpuinfo_cpu_stat` is
// itself DSL — it owns the OnceCell singleton, and a function-local
// static IS expressible in the DSL (`static NAME: T = init;`). The rest
// are the sampling kernels (syscalls + /proc parsing, all #ifdef-split by
// platform). Definitions in the impl namespace at the bottom of this file.
rusty::Vec<double> cpuinfo_cpu_stat();
struct CPUInfo;
void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks);
std::string cpuinfo_read_proc(const std::string& path);
unsigned long cpuinfo_parse_ulong(const std::string& tok);
std::string cpuinfo_net_path(const std::string& pid);
std::string cpuinfo_stat_path(const std::string& pid);
struct CPUInfo;
void cpuinfo_get_network(CPUInfo& info, const std::string& pid,
                         rusty::Vec<double>* result, int64_t ticks);
void cpuinfo_get_memory(CPUInfo& info, const std::string& pid,
                        rusty::Vec<double>* result, int64_t ticks);

// `CPUInfo` — process-level cpu/network/memory sampling history.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * Everything that was private is now public (DSL structs have no
//     access specifiers); the only external entry point remains the
//     static `cpu_stat()`, and the only construction site is the
//     singleton inside cpuinfo_cpu_stat (which value-initializes via a
//     designated-init literal, replacing the old in-class `= {}`
//     field initializers).
//   * new_/get_cpu_stat/get_network/get_memory moved to cpuinfo_*
//     free fns (their bodies are platform-#ifdef syscall + /proc
//     parsing kernels).
#if RUSTYCPP_RUST
// Preserve the pre-conversion C++ trait surface: the old opaque C-array
// aliases kept CPUInfo out of rusty::is_send/is_sync. Rust still derives its
// native auto traits because this rustc-visible cfg predicate is always false.
#[cfg_attr(any(), cpp_no_auto_traits)]
struct CPUInfo {
    last_bytes_rxed: [u64; 10],
    last_bytes_txed: [u64; 10],
    last_mem_usage: [u64; 10],
    last_ticks_: [i64; 10],
    last_user_ticks_: [i64; 10],
    last_kernel_ticks_: [i64; 10],
    last_cpu: f64,
    last_txed: f64,
    last_rxed: f64,
    last_mem: f64,
    total_mem: i64,
    page_size: i64,
    index: i32,
    pid_: i32,
    // Mutex protecting the sample-history fields above (payload bool is
    // an unused placeholder; see get_cpu_stat below).
    mtx_: Mutex<bool>,
}

impl CPUInfo {
    // Sample cpu/net/mem deltas against the singleton history.
    fn cpu_stat() -> Vec<f64> {
        cpuinfo_cpu_stat()
    }

    // Ring-buffer delta computation over the sample history. The
    // syscall sampling (process_times) and the /proc parsers stay in
    // kernels; everything else is plain control flow.
    fn get_cpu_stat(&mut self) -> Vec<f64> {
        let _guard = self.mtx_.lock().unwrap();

        let mut result = Vec::<f64>::new();
        let sample = rusty::sys::process::process_times();
        let ticks = sample.wall_ticks;
        let stime = sample.system_ticks;
        let utime = sample.user_ticks;

        let mut last_ticks: i64 = 0;
        if self.index < 10 {
            last_ticks = self.last_ticks_[self.index - 1];
        } else {
            last_ticks = self.last_ticks_[9];
        }

        cpuinfo_log_ticks(last_ticks, ticks);
        if ticks <= last_ticks + 60 {
            if self.index < 10 {
                result.push(-1.0);
                result.push(-1.0);
                result.push(-1.0);
                result.push(-1.0);
            } else {
                result.push(self.last_cpu);
                result.push(self.last_txed);
                result.push(self.last_rxed);
                result.push(self.last_mem);
            }
            return result;
        }

        if self.index < 10 {
            self.last_kernel_ticks_[self.index] = stime;
            self.last_user_ticks_[self.index] = utime;
            self.last_ticks_[self.index] = ticks;
            self.index += 1;
        } else {
            let mut i = 0;
            while i < 9 {
                self.last_kernel_ticks_[i] = self.last_kernel_ticks_[i + 1];
                self.last_user_ticks_[i] = self.last_user_ticks_[i + 1];
                self.last_ticks_[i] = self.last_ticks_[i + 1];
                i += 1;
            }
            self.last_kernel_ticks_[9] = stime;
            self.last_user_ticks_[9] = utime;
            self.last_ticks_[9] = ticks;
        }

        let mut cpu_total = 0.0;
        if self.index < 10 {
            cpu_total = -1.0;
        } else {
            let busy = (stime - self.last_kernel_ticks_[8]) + (utime - self.last_user_ticks_[8]);
            cpu_total = (busy as f64) / ((ticks - self.last_ticks_[8]) as f64);
        }
        self.last_cpu = cpu_total;

        if self.index < 10 {
            result.push(-1.0);
        } else {
            result.push(cpu_total);
        }

        cpuinfo_get_network(self, std::to_string(self.pid_), &mut result, ticks);
        cpuinfo_get_memory(self, std::to_string(self.pid_), &mut result, ticks);
        result
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.info version=1 rust_sha256=e65d90cff22552bf578c6da5a7cccd7540e235c6c9cd22746db10f0fe3f57340*/
struct CPUInfo;

struct CPUInfo {
    std::array<uint64_t, 10> last_bytes_rxed;
    std::array<uint64_t, 10> last_bytes_txed;
    std::array<uint64_t, 10> last_mem_usage;
    std::array<int64_t, 10> last_ticks_;
    std::array<int64_t, 10> last_user_ticks_;
    std::array<int64_t, 10> last_kernel_ticks_;
    double last_cpu;
    double last_txed;
    double last_rxed;
    double last_mem;
    int64_t total_mem;
    int64_t page_size;
    int32_t index;
    int32_t pid_;
    rusty::Mutex<bool> mtx_;

    static rusty::Vec<double> cpu_stat();
    rusty::Vec<double> get_cpu_stat();
};


rusty::Vec<double> CPUInfo::cpu_stat() {
    return cpuinfo_cpu_stat();
}

rusty::Vec<double> CPUInfo::get_cpu_stat() {
    auto _guard = this->mtx_.lock().unwrap();
    auto result = rusty::Vec<double>::new_();
    const auto sample = rusty::sys::process::process_times();
    auto ticks = std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.wall_ticks); }) { return (__r.wall_ticks); } else if constexpr (requires { (__r.wall_ticks_field); }) { return (__r.wall_ticks_field); } else if constexpr (requires { ((*__r).wall_ticks); }) { return ((*__r).wall_ticks); } else { return ((*__r).wall_ticks_field); } }(sample));
    auto stime = std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.system_ticks); }) { return (__r.system_ticks); } else if constexpr (requires { (__r.system_ticks_field); }) { return (__r.system_ticks_field); } else if constexpr (requires { ((*__r).system_ticks); }) { return ((*__r).system_ticks); } else { return ((*__r).system_ticks_field); } }(sample));
    auto utime = std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.user_ticks); }) { return (__r.user_ticks); } else if constexpr (requires { (__r.user_ticks_field); }) { return (__r.user_ticks_field); } else if constexpr (requires { ((*__r).user_ticks); }) { return ((*__r).user_ticks); } else { return ((*__r).user_ticks_field); } }(sample));
    int64_t last_ticks = static_cast<int64_t>(0);
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        last_ticks = this->last_ticks_.at(rusty::detail::deref_if_pointer_like(this->index) - 1);
    } else {
        last_ticks = this->last_ticks_.at(static_cast<size_t>(9));
    }
    cpuinfo_log_ticks(std::move(last_ticks), std::move(ticks));
    if (rusty::detail::deref_if_pointer_like(ticks) <= (rusty::detail::deref_if_pointer_like(last_ticks) + 60)) {
        if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
            result.push(-1.0);
            result.push(-1.0);
            result.push(-1.0);
            result.push(-1.0);
        } else {
            result.push(this->last_cpu);
            result.push(this->last_txed);
            result.push(this->last_rxed);
            result.push(this->last_mem);
        }
        return std::move(result);
    }
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        this->last_kernel_ticks_.at(this->index) = std::move(stime);
        this->last_user_ticks_.at(this->index) = std::move(utime);
        this->last_ticks_.at(this->index) = std::move(ticks);
        this->index += 1;
    } else {
        auto i = 0;
        while (rusty::detail::deref_if_pointer_like(i) < 9) {
            this->last_kernel_ticks_.at(i) = this->last_kernel_ticks_.at(rusty::detail::deref_if_pointer_like(i) + 1);
            this->last_user_ticks_.at(i) = this->last_user_ticks_.at(rusty::detail::deref_if_pointer_like(i) + 1);
            this->last_ticks_.at(i) = this->last_ticks_.at(rusty::detail::deref_if_pointer_like(i) + 1);
            rusty::detail::deref_if_pointer_like(i) += 1;
        }
        this->last_kernel_ticks_.at(static_cast<size_t>(9)) = std::move(stime);
        this->last_user_ticks_.at(static_cast<size_t>(9)) = std::move(utime);
        this->last_ticks_.at(static_cast<size_t>(9)) = std::move(ticks);
    }
    auto cpu_total = 0.0;
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        cpu_total = -1.0;
    } else {
        const auto busy = ((rusty::detail::deref_if_pointer_like(stime) - this->last_kernel_ticks_.at(static_cast<size_t>(8)))) + ((rusty::detail::deref_if_pointer_like(utime) - this->last_user_ticks_.at(static_cast<size_t>(8))));
        cpu_total = ((static_cast<double>(busy))) / ((static_cast<double>((rusty::detail::deref_if_pointer_like(ticks) - this->last_ticks_.at(static_cast<size_t>(8))))));
    }
    this->last_cpu = std::move(cpu_total);
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        result.push(-1.0);
    } else {
        result.push(std::move(cpu_total));
    }
    cpuinfo_get_network((*this), std::to_string(this->pid_), &result, std::move(ticks));
    cpuinfo_get_memory((*this), std::to_string(this->pid_), &result, std::move(ticks));
    return std::move(result);
}
/*RUSTYCPP:GEN-END id=cpuinfo.info*/


// /proc parsers, authored as inline Rust DSL over the std-faithful
// rusty::sys::fs::read_to_string (via the cpuinfo_read_proc bridge
// kernel — Result<_, io::Error> handling is a DSL wall) plus
// find/substr string walking. Tokenization mirrors the legacy strtok
// exactly (runs of ' ' collapse); malformed/short input now yields 0
// from an empty token instead of the legacy strtoul(NULL) crash.
#if RUSTYCPP_RUST
fn cpuinfo_nth_line(content: &std::string, n: i32) -> std::string {
    let mut pos: usize = 0;
    let mut k = 0;
    while k < n {
        let nl = content.find("\n", pos);
        if nl == std::string::npos {
            return std::string();
        }
        pos = nl + 1;
        k += 1;
    }
    let end = content.find("\n", pos);
    if end == std::string::npos {
        return content.substr(pos);
    }
    content.substr(pos, end - pos)
}

fn cpuinfo_nth_field(line: &std::string, n: i32) -> std::string {
    let mut pos = line.find_first_not_of(" ", 0);
    let mut k = 0;
    while pos != std::string::npos {
        let end = line.find(" ", pos);
        if k == n {
            if end == std::string::npos {
                return line.substr(pos);
            }
            return line.substr(pos, end - pos);
        }
        if end == std::string::npos {
            return std::string();
        }
        pos = line.find_first_not_of(" ", end);
        k += 1;
    }
    std::string()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.string_helpers version=1 rust_sha256=e3013e6103e65d5e8f9d8ec49871e430be656eefe398632fb94c10e2d4c443a7*/
std::string cpuinfo_nth_line(const std::string& content, int32_t n);
std::string cpuinfo_nth_field(const std::string& line, int32_t n);

std::string cpuinfo_nth_line(const std::string& content, int32_t n) {
    size_t pos = static_cast<size_t>(0);
    auto k = 0;
    while (rusty::detail::deref_if_pointer_like(k) < rusty::detail::deref_if_pointer_like(n)) {
        const auto nl = content.find("\n", std::move(pos));
        if (rusty::detail::deref_if_pointer_like(nl) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
            return std::string();
        }
        pos = rusty::detail::deref_if_pointer_like(nl) + static_cast<size_t>(1);
        rusty::detail::deref_if_pointer_like(k) += 1;
    }
    const auto end = content.find("\n", std::move(pos));
    if (rusty::detail::deref_if_pointer_like(end) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
        return content.substr(std::move(pos));
    }
    return content.substr(std::move(pos), rusty::detail::deref_if_pointer_like(end) - rusty::detail::deref_if_pointer_like(pos));
}

std::string cpuinfo_nth_field(const std::string& line, int32_t n) {
    auto pos = line.find_first_not_of(" ", 0);
    auto k = 0;
    while (rusty::detail::deref_if_pointer_like(pos) != rusty::detail::deref_if_pointer_like(std::string::npos)) {
        const auto end = line.find(" ", std::move(pos));
        if (rusty::detail::deref_if_pointer_like(k) == rusty::detail::deref_if_pointer_like(n)) {
            if (rusty::detail::deref_if_pointer_like(end) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
                return line.substr(std::move(pos));
            }
            return line.substr(std::move(pos), rusty::detail::deref_if_pointer_like(end) - rusty::detail::deref_if_pointer_like(pos));
        }
        if (rusty::detail::deref_if_pointer_like(end) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
            return std::string();
        }
        pos = line.find_first_not_of(" ", std::move(end));
        rusty::detail::deref_if_pointer_like(k) += 1;
    }
    return std::string();
}
/*RUSTYCPP:GEN-END id=cpuinfo.string_helpers*/


// @safe - path builders for the DSL (operator+ on std::string has no
// DSL spelling).
#if RUSTYCPP_RUST
fn cpuinfo_net_path(pid: &std::string) -> std::string {
    format!("/proc/{}/net/dev", pid)
}

fn cpuinfo_stat_path(pid: &std::string) -> std::string {
    format!("/proc/{}/stat", pid)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.4 version=1 rust_sha256=0b0774e1025b07413622a49e53c531d09ae930d5968971fa821ff0b6fcd70347*/
std::string cpuinfo_net_path(const std::string& pid);
std::string cpuinfo_stat_path(const std::string& pid);

std::string cpuinfo_net_path(const std::string& pid) {
    return std::format("/proc/{}/net/dev" , pid);
}

std::string cpuinfo_stat_path(const std::string& pid) {
    return std::format("/proc/{}/stat" , pid);
}
/*RUSTYCPP:GEN-END id=cpuinfo.4*/

} // export namespace rrr

// @safe - impl namespace: the cpuinfo_* sampling kernels. Each carries
// per-fn `// @unsafe` for syscalls (via the @safe rusty::sys::process
// helpers) and the tiny bridge/parse kernels (Result bridge, strtoul +
// operator>> chains). SP-5 (Cursor) remains the eventual refactor
// target for the two parsers.
namespace rrr {

// Forward declaration: the DSL factory below calls this kernel, whose
// definition sits further down with the other syscall kernels.
CPUInfo cpuinfo_blank();

// RELOCATED from the export namespace (Goal 0). The two /proc parsers are
// DEFINED here in the impl namespace; their DECLARATIONS stay exported
// above (cpuinfo.cpp:51-54), so exported callers still resolve and the
// entity keeps its exported linkage -- the same implicitly-exported
// redeclaration pattern logging.cpp uses for log_basename.
//
// The move is what lets `cpuinfo_new` join this DSL block: it calls
// `cpuinfo_get_network(&mut info, ..)`, and a CROSS-block `&mut` argument
// lowers to `&info` (a hard error), so the caller has to share the block.
// Merging into the block while it sat in the export namespace would have
// dragged cpuinfo_new into the module interface; relocating first keeps
// the interface exactly as it was.

// tx/rx sampling from /proc/<pid>/net/dev (4th line; strtok-era field
// numbering: field 1 and field 9 after the interface token).
#if RUSTYCPP_RUST
fn cpuinfo_get_network(info: &mut CPUInfo, pid: &std::string,
                       result: *mut rusty::Vec<f64>, ticks: i64) {
    let content = cpuinfo_read_proc(cpuinfo_net_path(pid));
    let line = cpuinfo_nth_line(content, 3);
    let txed = cpuinfo_parse_ulong(cpuinfo_nth_field(line, 1)) as u64;
    let rxed = cpuinfo_parse_ulong(cpuinfo_nth_field(line, 9)) as u64;

    let mut tx_total: f64 = -1.0;
    let mut rx_total: f64 = -1.0;

    if info.index < 10 {
        info.last_bytes_txed[info.index] = txed;
        info.last_bytes_rxed[info.index] = rxed;
    } else {
        let mut j = 0;
        while j < 9 {
            info.last_bytes_txed[j] = info.last_bytes_txed[j + 1];
            info.last_bytes_rxed[j] = info.last_bytes_rxed[j + 1];
            j += 1;
        }
        info.last_bytes_txed[9] = txed;
        info.last_bytes_rxed[9] = rxed;
    }

    if ticks != info.last_ticks_[0] {
        if info.index >= 10 {
            // Legacy integer division preserved (rates truncate).
            tx_total = ((txed - info.last_bytes_txed[8]) / ((ticks - info.last_ticks_[8]) as u64)) as f64;
            rx_total = ((rxed - info.last_bytes_rxed[8]) / ((ticks - info.last_ticks_[8]) as u64)) as f64;
        }
    }

    result.push(tx_total);
    result.push(rx_total);
    info.last_txed = tx_total;
    info.last_rxed = rx_total;
}

// rss sampling from /proc/<pid>/stat (whitespace field 24, 1-indexed).
fn cpuinfo_get_memory(info: &mut CPUInfo, pid: &std::string,
                      result: *mut rusty::Vec<f64>, ticks: i64) {
    let content = cpuinfo_read_proc(cpuinfo_stat_path(pid));
    let line = cpuinfo_nth_line(content, 0);
    let rss = cpuinfo_parse_ulong(cpuinfo_nth_field(line, 23)) as i64;
    let mem_usage = (rss * info.page_size) as f64;
    let mut mem_total: f64 = -1.0;

    if info.index < 10 {
        info.last_mem_usage[info.index] = mem_usage as u64;
    } else {
        let mut j = 0;
        while j < 9 {
            info.last_mem_usage[j] = info.last_mem_usage[j + 1];
            j += 1;
        }
        info.last_mem_usage[9] = mem_usage as u64;
    }

    if ticks != info.last_ticks_[0] {
        if info.index >= 10 {
            // Double division here (unlike the network path) — legacy.
            mem_total = (mem_usage - (info.last_mem_usage[8] as f64)) / ((ticks - info.last_ticks_[8]) as f64);
        }
    }

    result.push(mem_total);
    info.last_mem = mem_total;
}

// The CPUInfo factory, in the SAME block as the two parsers it calls --
// a CROSS-block `&mut info` argument lowers to `&info`, which is a hard
// error, so the caller has to share the block. (That constraint is why
// the block was relocated out of the export namespace first: merging
// while it sat there would have dragged this factory into the module
// interface.)
//
// The platform split is item-level `#[cfg]` on two same-named fns, which
// emits real `#if defined(__linux__)` guards. It is NOT statement-level
// #[cfg] -- that spelling silently miscompiles, picking one arm on every
// platform.
#[cfg(target_os = "linux")]
fn cpuinfo_new() -> CPUInfo {
    let mut info = cpuinfo_blank();
    let mut result = Vec::<f64>::new();

    let mem_info = rusty::sys::process::sysinfo();
    // `total_ram_bytes` is already scaled by mem_unit.
    info.total_mem = (mem_info.total_ram_bytes / 1024) as i64;
    log_line(Log::DEBUG, 0i32, core::ptr::null(),
             std::format("total amount of ram is: {}", info.total_mem));

    info.page_size = rusty::sys::process::sysconf(_SC_PAGE_SIZE) / 1024;

    let ticks = rusty::sys::process::process_times();
    // `idx` is bound to a local ON PURPOSE. Spelling the subscript as
    // `info.last_ticks_[info.index]` makes the transpiler emit a
    // field-access lambda for BOTH operands, producing `...)[[&](auto&&...`
    // -- two consecutive `[`, which C++ parses as an attribute:
    // "C++11 only allows consecutive left square brackets when
    // introducing an attribute". A plain local for the index avoids it.
    let idx = info.index;
    info.last_ticks_[idx]        = ticks.wall_ticks as i64;
    info.last_kernel_ticks_[idx] = ticks.system_ticks as i64;
    info.last_user_ticks_[idx]   = ticks.user_ticks as i64;

    info.pid_ = rusty::sys::process::getpid();
    let pid_str = std::to_string(info.pid_);
    let t0 = info.last_ticks_[idx] as i64;
    cpuinfo_get_network(&mut info, &pid_str, &raw mut result, t0);
    cpuinfo_get_memory(&mut info, &pid_str, &raw mut result, t0);

    info.index += 1;
    info
}

#[cfg(not(target_os = "linux"))]
fn cpuinfo_new() -> CPUInfo {
    let mut info = cpuinfo_blank();
    info.last_cpu = 0.0;
    info.last_txed = 0.0;
    info.last_rxed = 0.0;
    info.last_mem = 0.0;
    info.total_mem = 0;
    info.page_size = 0;
    info.index = 0;
    info.pid_ = rusty::sys::process::getpid();
    info
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.parsers version=1 rust_sha256=d7be545115700d0350191c31b536e21621160faff9033474f96741d580833a52*/
void cpuinfo_get_network(CPUInfo& info, const std::string& pid, rusty::Vec<double>* result, int64_t ticks) {
    CPUInfo* info_shadow1 = &info;
    const auto content = cpuinfo_read_proc(cpuinfo_net_path(pid));
    const auto line = cpuinfo_nth_line(std::move(content), 3);
    auto txed = static_cast<uint64_t>(cpuinfo_parse_ulong(cpuinfo_nth_field(std::move(line), 1)));
    auto rxed = static_cast<uint64_t>(cpuinfo_parse_ulong(cpuinfo_nth_field(std::move(line), 9)));
    double tx_total = -1.0;
    double rx_total = -1.0;
    if (rusty::detail::deref_if_pointer_like((*info_shadow1).index) < 10) {
        (*info_shadow1).last_bytes_txed[(*info_shadow1).index] = std::move(txed);
        (*info_shadow1).last_bytes_rxed[(*info_shadow1).index] = std::move(rxed);
    } else {
        auto j = 0;
        while (rusty::detail::deref_if_pointer_like(j) < 9) {
            (*info_shadow1).last_bytes_txed[j] = (*info_shadow1).last_bytes_txed[rusty::detail::deref_if_pointer_like(j) + 1];
            (*info_shadow1).last_bytes_rxed[j] = (*info_shadow1).last_bytes_rxed[rusty::detail::deref_if_pointer_like(j) + 1];
            rusty::detail::deref_if_pointer_like(j) += 1;
        }
        (*info_shadow1).last_bytes_txed[static_cast<size_t>(9)] = std::move(txed);
        (*info_shadow1).last_bytes_rxed[static_cast<size_t>(9)] = std::move(rxed);
    }
    if (rusty::detail::deref_if_pointer_like(ticks) != (*info_shadow1).last_ticks_[static_cast<size_t>(0)]) {
        if (rusty::detail::deref_if_pointer_like((*info_shadow1).index) >= 10) {
            tx_total = static_cast<double>((((rusty::detail::deref_if_pointer_like(txed) - (*info_shadow1).last_bytes_txed[static_cast<size_t>(8)])) / ((static_cast<uint64_t>((rusty::detail::deref_if_pointer_like(ticks) - (*info_shadow1).last_ticks_[static_cast<size_t>(8)]))))));
            rx_total = static_cast<double>((((rusty::detail::deref_if_pointer_like(rxed) - (*info_shadow1).last_bytes_rxed[static_cast<size_t>(8)])) / ((static_cast<uint64_t>((rusty::detail::deref_if_pointer_like(ticks) - (*info_shadow1).last_ticks_[static_cast<size_t>(8)]))))));
        }
    }
    result->push(std::move(tx_total));
    result->push(std::move(rx_total));
    (*info_shadow1).last_txed = std::move(tx_total);
    (*info_shadow1).last_rxed = std::move(rx_total);
}

void cpuinfo_get_memory(CPUInfo& info, const std::string& pid, rusty::Vec<double>* result, int64_t ticks) {
    CPUInfo* info_shadow1 = &info;
    const auto content = cpuinfo_read_proc(cpuinfo_stat_path(pid));
    const auto line = cpuinfo_nth_line(std::move(content), 0);
    const auto rss = static_cast<int64_t>(cpuinfo_parse_ulong(cpuinfo_nth_field(std::move(line), 23)));
    const auto mem_usage = static_cast<double>((rusty::detail::deref_if_pointer_like(rss) * rusty::detail::deref_if_pointer_like((*info_shadow1).page_size)));
    double mem_total = -1.0;
    if (rusty::detail::deref_if_pointer_like((*info_shadow1).index) < 10) {
        (*info_shadow1).last_mem_usage[(*info_shadow1).index] = static_cast<uint64_t>(mem_usage);
    } else {
        auto j = 0;
        while (rusty::detail::deref_if_pointer_like(j) < 9) {
            (*info_shadow1).last_mem_usage[j] = (*info_shadow1).last_mem_usage[rusty::detail::deref_if_pointer_like(j) + 1];
            rusty::detail::deref_if_pointer_like(j) += 1;
        }
        (*info_shadow1).last_mem_usage[static_cast<size_t>(9)] = static_cast<uint64_t>(mem_usage);
    }
    if (rusty::detail::deref_if_pointer_like(ticks) != (*info_shadow1).last_ticks_[static_cast<size_t>(0)]) {
        if (rusty::detail::deref_if_pointer_like((*info_shadow1).index) >= 10) {
            mem_total = ((rusty::detail::deref_if_pointer_like(mem_usage) - ((static_cast<double>((*info_shadow1).last_mem_usage[static_cast<size_t>(8)]))))) / ((static_cast<double>((rusty::detail::deref_if_pointer_like(ticks) - (*info_shadow1).last_ticks_[static_cast<size_t>(8)]))));
        }
    }
    result->push(std::move(mem_total));
    (*info_shadow1).last_mem = std::move(mem_total);
}

#if defined(__linux__)
CPUInfo cpuinfo_new() {
    auto info = cpuinfo_blank();
    auto result = rusty::Vec<double>::new_();
    const auto mem_info = rusty::sys::process::sysinfo();
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.total_mem); }) { return (__r.total_mem); } else if constexpr (requires { (__r.total_mem_field); }) { return (__r.total_mem_field); } else if constexpr (requires { ((*__r).total_mem); }) { return ((*__r).total_mem); } else { return ((*__r).total_mem_field); } }(info) = static_cast<int64_t>((rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.total_ram_bytes); }) { return (__r.total_ram_bytes); } else if constexpr (requires { (__r.total_ram_bytes_field); }) { return (__r.total_ram_bytes_field); } else if constexpr (requires { ((*__r).total_ram_bytes); }) { return ((*__r).total_ram_bytes); } else { return ((*__r).total_ram_bytes_field); } }(mem_info)) / 1024));
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("total amount of ram is: {}", std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.total_mem); }) { return (__r.total_mem); } else if constexpr (requires { (__r.total_mem_field); }) { return (__r.total_mem_field); } else if constexpr (requires { ((*__r).total_mem); }) { return ((*__r).total_mem); } else { return ((*__r).total_mem_field); } }(info))));
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.page_size); }) { return (__r.page_size); } else if constexpr (requires { (__r.page_size_field); }) { return (__r.page_size_field); } else if constexpr (requires { ((*__r).page_size); }) { return ((*__r).page_size); } else { return ((*__r).page_size_field); } }(info) = rusty::sys::process::sysconf(_SC_PAGE_SIZE) / 1024;
    const auto ticks = rusty::sys::process::process_times();
    const auto idx = std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.index); }) { return (__r.index); } else if constexpr (requires { (__r.index_field); }) { return (__r.index_field); } else if constexpr (requires { ((*__r).index); }) { return ((*__r).index); } else { return ((*__r).index_field); } }(info));
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_ticks_); }) { return (__r.last_ticks_); } else if constexpr (requires { (__r.last_ticks__field); }) { return (__r.last_ticks__field); } else if constexpr (requires { ((*__r).last_ticks_); }) { return ((*__r).last_ticks_); } else { return ((*__r).last_ticks__field); } }(info)[idx] = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.wall_ticks); }) { return (__r.wall_ticks); } else if constexpr (requires { (__r.wall_ticks_field); }) { return (__r.wall_ticks_field); } else if constexpr (requires { ((*__r).wall_ticks); }) { return ((*__r).wall_ticks); } else { return ((*__r).wall_ticks_field); } }(ticks));
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_kernel_ticks_); }) { return (__r.last_kernel_ticks_); } else if constexpr (requires { (__r.last_kernel_ticks__field); }) { return (__r.last_kernel_ticks__field); } else if constexpr (requires { ((*__r).last_kernel_ticks_); }) { return ((*__r).last_kernel_ticks_); } else { return ((*__r).last_kernel_ticks__field); } }(info)[idx] = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.system_ticks); }) { return (__r.system_ticks); } else if constexpr (requires { (__r.system_ticks_field); }) { return (__r.system_ticks_field); } else if constexpr (requires { ((*__r).system_ticks); }) { return ((*__r).system_ticks); } else { return ((*__r).system_ticks_field); } }(ticks));
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_user_ticks_); }) { return (__r.last_user_ticks_); } else if constexpr (requires { (__r.last_user_ticks__field); }) { return (__r.last_user_ticks__field); } else if constexpr (requires { ((*__r).last_user_ticks_); }) { return ((*__r).last_user_ticks_); } else { return ((*__r).last_user_ticks__field); } }(info)[idx] = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.user_ticks); }) { return (__r.user_ticks); } else if constexpr (requires { (__r.user_ticks_field); }) { return (__r.user_ticks_field); } else if constexpr (requires { ((*__r).user_ticks); }) { return ((*__r).user_ticks); } else { return ((*__r).user_ticks_field); } }(ticks));
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.pid_); }) { return (__r.pid_); } else if constexpr (requires { (__r.pid__field); }) { return (__r.pid__field); } else if constexpr (requires { ((*__r).pid_); }) { return ((*__r).pid_); } else { return ((*__r).pid__field); } }(info) = rusty::sys::process::getpid();
    const auto pid_str = std::to_string(std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.pid_); }) { return (__r.pid_); } else if constexpr (requires { (__r.pid__field); }) { return (__r.pid__field); } else if constexpr (requires { ((*__r).pid_); }) { return ((*__r).pid_); } else { return ((*__r).pid__field); } }(info)));
    auto t0 = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_ticks_); }) { return (__r.last_ticks_); } else if constexpr (requires { (__r.last_ticks__field); }) { return (__r.last_ticks__field); } else if constexpr (requires { ((*__r).last_ticks_); }) { return ((*__r).last_ticks_); } else { return ((*__r).last_ticks__field); } }(info)[idx]);
    cpuinfo_get_network(rusty::detail::deref_if_pointer_like(info), pid_str, &result, std::move(t0));
    cpuinfo_get_memory(rusty::detail::deref_if_pointer_like(info), pid_str, &result, std::move(t0));
    rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.index); }) { return (__r.index); } else if constexpr (requires { (__r.index_field); }) { return (__r.index_field); } else if constexpr (requires { ((*__r).index); }) { return ((*__r).index); } else { return ((*__r).index_field); } }(info)) += 1;
    return std::move(info);
}
#endif  // defined(__linux__)

#if !(defined(__linux__))
CPUInfo cpuinfo_new() {
    auto info = cpuinfo_blank();
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_cpu); }) { return (__r.last_cpu); } else if constexpr (requires { (__r.last_cpu_field); }) { return (__r.last_cpu_field); } else if constexpr (requires { ((*__r).last_cpu); }) { return ((*__r).last_cpu); } else { return ((*__r).last_cpu_field); } }(info) = 0.0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_txed); }) { return (__r.last_txed); } else if constexpr (requires { (__r.last_txed_field); }) { return (__r.last_txed_field); } else if constexpr (requires { ((*__r).last_txed); }) { return ((*__r).last_txed); } else { return ((*__r).last_txed_field); } }(info) = 0.0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_rxed); }) { return (__r.last_rxed); } else if constexpr (requires { (__r.last_rxed_field); }) { return (__r.last_rxed_field); } else if constexpr (requires { ((*__r).last_rxed); }) { return ((*__r).last_rxed); } else { return ((*__r).last_rxed_field); } }(info) = 0.0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.last_mem); }) { return (__r.last_mem); } else if constexpr (requires { (__r.last_mem_field); }) { return (__r.last_mem_field); } else if constexpr (requires { ((*__r).last_mem); }) { return ((*__r).last_mem); } else { return ((*__r).last_mem_field); } }(info) = 0.0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.total_mem); }) { return (__r.total_mem); } else if constexpr (requires { (__r.total_mem_field); }) { return (__r.total_mem_field); } else if constexpr (requires { ((*__r).total_mem); }) { return ((*__r).total_mem); } else { return ((*__r).total_mem_field); } }(info) = 0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.page_size); }) { return (__r.page_size); } else if constexpr (requires { (__r.page_size_field); }) { return (__r.page_size_field); } else if constexpr (requires { ((*__r).page_size); }) { return ((*__r).page_size); } else { return ((*__r).page_size_field); } }(info) = 0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.index); }) { return (__r.index); } else if constexpr (requires { (__r.index_field); }) { return (__r.index_field); } else if constexpr (requires { ((*__r).index); }) { return ((*__r).index); } else { return ((*__r).index_field); } }(info) = 0;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.pid_); }) { return (__r.pid_); } else if constexpr (requires { (__r.pid__field); }) { return (__r.pid__field); } else if constexpr (requires { ((*__r).pid_); }) { return ((*__r).pid_); } else { return ((*__r).pid__field); } }(info) = rusty::sys::process::getpid();
    return std::move(info);
}
#endif  // !(defined(__linux__))
/*RUSTYCPP:GEN-END id=cpuinfo.parsers*/

// (anonymous namespace dissolved: the kernels need module linkage so the
// exported declarations above unify with these definitions)


// @safe - Result<_, io::Error> bridge over the std-faithful
// rusty::sys::fs::read_to_string ("" on any error — matches the legacy
// silent-ifstream-failure behavior).
// Authored as inline Rust DSL — plain Result handling.
#if RUSTYCPP_RUST
fn cpuinfo_read_proc(path: &std::string) -> std::string {
    let r = rusty::sys::fs::read_to_string(path);
    if r.is_err() {
        return std::string();
    }
    return r.unwrap();
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.read_proc version=1 rust_sha256=839a3fd41b3c88a413dfd92bb7a6c80510cba5e818f902f9da152808f96b88e7*/
std::string cpuinfo_read_proc(const std::string& path);

std::string cpuinfo_read_proc(const std::string& path) {
    auto r = rusty::sys::fs::read_to_string(path);
    if (r.is_err()) {
        return std::string();
    }
    return r.unwrap();
}
/*RUSTYCPP:GEN-END id=cpuinfo.read_proc*/

// @unsafe - strtoul over the token bytes (empty token parses to 0,
// replacing the legacy strtoul(NULL) crash on short input).
#if RUSTYCPP_RUST
fn cpuinfo_parse_ulong(tok: &std::string) -> usize {
    unsafe { strtoul(tok.c_str(), std::ptr::null_mut(), 0) }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.8 version=1 rust_sha256=7dade42978a7d6f745a3e1dfb8e2b07fc86b27bfe37c3b7a729060b897f58959*/
size_t cpuinfo_parse_ulong(const std::string& tok);

size_t cpuinfo_parse_ulong(const std::string& tok) {
    // @unsafe
    {
        return strtoul(tok.c_str(), rusty::ptr::null_mut(), 0);
    }
}
/*RUSTYCPP:GEN-END id=cpuinfo.8*/

// Complete Rust construction of an empty sample history. Fixed-size Rust
// arrays lower to std::array with the same layout as the former C arrays on
// the supported Linux ABIs; spelling every field also keeps this valid Rust
// instead of relying on C++ partial designated-initializer semantics.
#if RUSTYCPP_RUST
fn cpuinfo_blank() -> CPUInfo {
    CPUInfo {
        last_bytes_rxed: [0u64; 10],
        last_bytes_txed: [0u64; 10],
        last_mem_usage: [0u64; 10],
        last_ticks_: [0i64; 10],
        last_user_ticks_: [0i64; 10],
        last_kernel_ticks_: [0i64; 10],
        last_cpu: 0.0,
        last_txed: 0.0,
        last_rxed: 0.0,
        last_mem: 0.0,
        total_mem: 0i64,
        page_size: 0i64,
        index: 0i32,
        pid_: 0i32,
        mtx_: rusty::Mutex::<bool>::new(false),
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.9 version=1 rust_sha256=b68096924d097a8e218cfcdcad99b115373143e7ab89088c70685c3c828e97d4*/
CPUInfo cpuinfo_blank() {
    return CPUInfo{.last_bytes_rxed = rusty::array_repeat(static_cast<uint64_t>(0), 10), .last_bytes_txed = rusty::array_repeat(static_cast<uint64_t>(0), 10), .last_mem_usage = rusty::array_repeat(static_cast<uint64_t>(0), 10), .last_ticks_ = rusty::array_repeat(static_cast<int64_t>(0), 10), .last_user_ticks_ = rusty::array_repeat(static_cast<int64_t>(0), 10), .last_kernel_ticks_ = rusty::array_repeat(static_cast<int64_t>(0), 10), .last_cpu = 0.0, .last_txed = 0.0, .last_rxed = 0.0, .last_mem = 0.0, .total_mem = static_cast<int64_t>(0), .page_size = static_cast<int64_t>(0), .index = static_cast<int32_t>(0), .pid_ = static_cast<int32_t>(0), .mtx_ = rusty::Mutex<bool>::new_(false)};
}
/*RUSTYCPP:GEN-END id=cpuinfo.9*/


// @unsafe - Log_debug varargs shim for the DSL delta method.
#if RUSTYCPP_RUST
fn cpuinfo_log_ticks(last_ticks: clock_t, ticks: clock_t) {
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("ticks: {} -> {}", last_ticks, ticks));
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.6 version=1 rust_sha256=3363f45e1f7e40f4bea593b9dcafa09fb5ac2e2de59b9f2645c000211bb3dae3*/
void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks);

void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks) {
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ticks: {} -> {}", std::move(last_ticks), std::move(ticks)));
}
/*RUSTYCPP:GEN-END id=cpuinfo.6*/

// @unsafe - Rust-idiomatic singleton accessor. Dereferences the raw
// pointer `OnceCell::get_mut()` hands back (non-null immediately after
// get_or_init, which rusty-cpp cannot prove).
//
// Equivalent in Rust:
//   static CPU_INFO: OnceLock<CpuInfo> = OnceLock::new();
//   pub fn cpu_stat() -> Vec<f64> {
//       CPU_INFO.get_or_init(CpuInfo::new).get_cpu_stat()
//   }
//
// Authored as inline Rust DSL: the function-local static IS expressible
// (`static NAME: T = init;` lowers to a block-scope C++ static). OnceCell
// is non-copyable AND non-movable, so it is value-initialized with an empty
// struct literal — the emitted `= rusty::OnceCell<CPUInfo>{}` is copy-init
// from a prvalue of the same type, i.e. guaranteed elision, no move ctor
// required. `get_mut()` returns a RAW pointer, hence the explicit deref.
#if RUSTYCPP_RUST
fn cpuinfo_cpu_stat() -> Vec<f64> {
    static inst: rusty::OnceCell<CPUInfo> = rusty::OnceCell::<CPUInfo> {};
    inst.get_or_init(|| cpuinfo_new());
    (*inst.get_mut()).get_cpu_stat()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.7 version=1 rust_sha256=e80ab216873c88412d503dda6f73f84e3e772388130f289c931ce54ab1a5f8fc*/
rusty::Vec<double> cpuinfo_cpu_stat();

rusty::Vec<double> cpuinfo_cpu_stat() {
    static rusty::OnceCell<CPUInfo> inst = rusty::OnceCell<CPUInfo>{};
    inst.get_or_init([&]() { return cpuinfo_new(); });
    return ((rusty::detail::deref_if_pointer_like(inst.get_mut()))).get_cpu_stat();
}
/*RUSTYCPP:GEN-END id=cpuinfo.7*/

}  // namespace rrr
