module;

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

// C-array sample-history field types, aliased so the DSL can spell
// them (the DSL type grammar has no array syntax).
using CpuUlongSamples = unsigned long[10];
using CpuClockSamples = clock_t[10];
using CpuPid = pid_t;

struct CPUInfo;

// Backing free fn for the DSL `cpu_stat` below — owns the OnceCell
// singleton and the sampling machinery (syscalls + /proc parsing, all
// #ifdef-split by platform, not DSL-expressible). Definitions in the
// impl namespace at the bottom of this file.
rusty::Vec<double> cpuinfo_cpu_stat();
struct CPUInfo;
void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks);
std::string cpuinfo_read_proc(const std::string& path);
std::string cpuinfo_empty_string();
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
struct CPUInfo {
    last_bytes_rxed: CpuUlongSamples,
    last_bytes_txed: CpuUlongSamples,
    last_mem_usage: CpuUlongSamples,
    last_ticks_: CpuClockSamples,
    last_user_ticks_: CpuClockSamples,
    last_kernel_ticks_: CpuClockSamples,
    last_cpu: f64,
    last_txed: f64,
    last_rxed: f64,
    last_mem: f64,
    total_mem: i64,
    page_size: i64,
    index: i32,
    pid_: CpuPid,
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
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.info version=1 rust_sha256=d1fd3c4b352819ceb8684ad0e6ac6e58fb48f856778ea91f2e10884158565d2b*/
struct CPUInfo;

struct CPUInfo {
    CpuUlongSamples last_bytes_rxed;
    CpuUlongSamples last_bytes_txed;
    CpuUlongSamples last_mem_usage;
    CpuClockSamples last_ticks_;
    CpuClockSamples last_user_ticks_;
    CpuClockSamples last_kernel_ticks_;
    double last_cpu;
    double last_txed;
    double last_rxed;
    double last_mem;
    int64_t total_mem;
    int64_t page_size;
    int32_t index;
    CpuPid pid_;
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
        last_ticks = this->last_ticks_[rusty::detail::deref_if_pointer_like(this->index) - 1];
    } else {
        last_ticks = this->last_ticks_[static_cast<size_t>(9)];
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
        this->last_kernel_ticks_[this->index] = std::move(stime);
        this->last_user_ticks_[this->index] = std::move(utime);
        this->last_ticks_[this->index] = std::move(ticks);
        this->index += 1;
    } else {
        auto i = 0;
        while (rusty::detail::deref_if_pointer_like(i) < 9) {
            this->last_kernel_ticks_[i] = this->last_kernel_ticks_[rusty::detail::deref_if_pointer_like(i) + 1];
            this->last_user_ticks_[i] = this->last_user_ticks_[rusty::detail::deref_if_pointer_like(i) + 1];
            this->last_ticks_[i] = this->last_ticks_[rusty::detail::deref_if_pointer_like(i) + 1];
            rusty::detail::deref_if_pointer_like(i) += 1;
        }
        this->last_kernel_ticks_[static_cast<size_t>(9)] = std::move(stime);
        this->last_user_ticks_[static_cast<size_t>(9)] = std::move(utime);
        this->last_ticks_[static_cast<size_t>(9)] = std::move(ticks);
    }
    auto cpu_total = 0.0;
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        cpu_total = -1.0;
    } else {
        const auto busy = ((rusty::detail::deref_if_pointer_like(stime) - this->last_kernel_ticks_[static_cast<size_t>(8)])) + ((rusty::detail::deref_if_pointer_like(utime) - this->last_user_ticks_[static_cast<size_t>(8)]));
        cpu_total = ((static_cast<double>(busy))) / ((static_cast<double>((rusty::detail::deref_if_pointer_like(ticks) - this->last_ticks_[static_cast<size_t>(8)]))));
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
            return cpuinfo_empty_string();
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
            return cpuinfo_empty_string();
        }
        pos = line.find_first_not_of(" ", end);
        k += 1;
    }
    cpuinfo_empty_string()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.string_helpers version=1 rust_sha256=16af2185742570e264f4c6b4164f3586b12d9ac5b68a47910b19929ea2b5ffd1*/
std::string cpuinfo_nth_line(const std::string& content, int32_t n);
std::string cpuinfo_nth_field(const std::string& line, int32_t n);

std::string cpuinfo_nth_line(const std::string& content, int32_t n) {
    size_t pos = static_cast<size_t>(0);
    auto k = 0;
    while (rusty::detail::deref_if_pointer_like(k) < rusty::detail::deref_if_pointer_like(n)) {
        const auto nl = content.find("\n", std::move(pos));
        if (rusty::detail::deref_if_pointer_like(nl) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
            return cpuinfo_empty_string();
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
            return cpuinfo_empty_string();
        }
        pos = line.find_first_not_of(" ", std::move(end));
        rusty::detail::deref_if_pointer_like(k) += 1;
    }
    return cpuinfo_empty_string();
}
/*RUSTYCPP:GEN-END id=cpuinfo.string_helpers*/

// tx/rx sampling from /proc/<pid>/net/dev (4th line; strtok-era field
// numbering: field 1 and field 9 after the interface token).
#if RUSTYCPP_RUST
fn cpuinfo_get_network(info: &mut CPUInfo, pid: &std::string,
                       result: *mut rusty::Vec<f64>, ticks: i64) {
    let content = cpuinfo_read_proc(cpuinfo_net_path(pid));
    let line = cpuinfo_nth_line(content, 3);
    let txed = cpuinfo_parse_ulong(cpuinfo_nth_field(line, 1));
    let rxed = cpuinfo_parse_ulong(cpuinfo_nth_field(line, 9));

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
#endif
/*RUSTYCPP:GEN-BEGIN id=cpuinfo.parsers version=1 rust_sha256=10ebbc8a2627a442b7e2b8b3ae9428b79aff988b50b2fc6c5932b1198a3c8cf4*/
void cpuinfo_get_network(CPUInfo& info, const std::string& pid, rusty::Vec<double>* result, int64_t ticks) {
    CPUInfo* info_shadow1 = &info;
    const auto content = cpuinfo_read_proc(cpuinfo_net_path(pid));
    const auto line = cpuinfo_nth_line(std::move(content), 3);
    auto txed = cpuinfo_parse_ulong(cpuinfo_nth_field(std::move(line), 1));
    auto rxed = cpuinfo_parse_ulong(cpuinfo_nth_field(std::move(line), 9));
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
/*RUSTYCPP:GEN-END id=cpuinfo.parsers*/

// @safe - path builders for the DSL (operator+ on std::string has no
// DSL spelling).
inline std::string cpuinfo_net_path(const std::string& pid) {
    return "/proc/" + pid + "/net/dev";
}
inline std::string cpuinfo_stat_path(const std::string& pid) {
    return "/proc/" + pid + "/stat";
}

} // export namespace rrr

// @safe - impl namespace: the cpuinfo_* sampling kernels. Each carries
// per-fn `// @unsafe` for syscalls (via the @safe rusty::sys::process
// helpers) and the tiny bridge/parse kernels (Result bridge, strtoul +
// operator>> chains). SP-5 (Cursor) remains the eventual refactor
// target for the two parsers.
namespace rrr {

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

// @unsafe - trivial factory for the DSL.
std::string cpuinfo_empty_string() { return std::string(); }

// @unsafe - strtoul over the token bytes (empty token parses to 0,
// replacing the legacy strtoul(NULL) crash on short input).
unsigned long cpuinfo_parse_ulong(const std::string& tok) {
    return strtoul(tok.c_str(), nullptr, 0);
}

// @unsafe - sysinfo / sysconf / times / getpid flow through the @safe
// rusty::sys::process::* helpers; dispatches into the @unsafe parsers.
// No lock is taken: the instance is local until returned (only the
// singleton in cpuinfo_cpu_stat constructs one, exactly once).
CPUInfo cpuinfo_new() {
    // Designated-init: mtx_ gets its placeholder payload; every other
    // member is value-initialized ({} per C++20 designated-init rules),
    // replacing the old in-class `= {}` field initializers.
    CPUInfo info{.mtx_ = rusty::Mutex<bool>(false)};
#ifdef __linux__
    rusty::Vec<double> result;

    const auto mem_info = rusty::sys::process::sysinfo();
    // `mem_info.total_ram_bytes` is already scaled by mem_unit.
    info.total_mem = static_cast<long long>(mem_info.total_ram_bytes / 1024);
    Log_debug("total amount of ram is: {}", info.total_mem);

    info.page_size = rusty::sys::process::sysconf(_SC_PAGE_SIZE) / 1024;

    const auto ticks = rusty::sys::process::process_times();
    info.last_ticks_[info.index]        = static_cast<clock_t>(ticks.wall_ticks);
    info.last_kernel_ticks_[info.index] = static_cast<clock_t>(ticks.system_ticks);
    info.last_user_ticks_[info.index]   = static_cast<clock_t>(ticks.user_ticks);

    info.pid_ = rusty::sys::process::getpid();
    cpuinfo_get_network(info, std::to_string(info.pid_), &result, static_cast<int64_t>(info.last_ticks_[info.index]));
    cpuinfo_get_memory(info, std::to_string(info.pid_), &result, static_cast<int64_t>(info.last_ticks_[info.index]));

    info.index++;
#else
    info.last_cpu = info.last_txed = info.last_rxed = info.last_mem = 0.0;
    info.total_mem = 0;
    info.page_size = 0;
    info.index = 0;
    info.pid_ = rusty::sys::process::getpid();
#endif
    return info;
}

// @unsafe - Log_debug varargs shim for the DSL delta method.
void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks) {
    Log_debug("ticks: {} -> {}", last_ticks, ticks);
}

// @safe - Rust-idiomatic singleton accessor.
//
// Equivalent in Rust:
//   static CPU_INFO: OnceLock<CpuInfo> = OnceLock::new();
//   pub fn cpu_stat() -> Vec<f64> {
//       CPU_INFO.get_or_init(CpuInfo::new).get_cpu_stat()
//   }
// @unsafe - FUNCTION-LOCAL STATIC (`static rusty::OnceCell<CPUInfo>`).
// The DSL has no construct for a static declared inside a function body
// (§7.24b); hoisting it to namespace scope would change lifetime, so it
// stays a kernel.
rusty::Vec<double> cpuinfo_cpu_stat() {
    static rusty::OnceCell<CPUInfo> inst;
    inst.get_or_init([]() -> CPUInfo { return cpuinfo_new(); });
    return inst.get_mut()->get_cpu_stat();
}

}  // namespace rrr
