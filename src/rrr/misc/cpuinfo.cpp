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
// times, getpid) and parse /proc files via std::ifstream +
// operator>> / strtok / strtoul. The `cpu_stat()` factory just hands
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
void cpuinfo_get_network(CPUInfo& self, const std::string& pid,
                         rusty::Vec<double>* result, clock_t ticks);
void cpuinfo_get_memory(CPUInfo& self, const std::string& pid,
                        rusty::Vec<double>* result, clock_t ticks);
void cpuinfo_log_ticks(clock_t last_ticks, clock_t ticks);

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
    const auto ticks = std::move(sample.wall_ticks);
    const auto stime = std::move(sample.system_ticks);
    const auto utime = std::move(sample.user_ticks);
    int64_t last_ticks = static_cast<int64_t>(0);
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        last_ticks = this->last_ticks_[rusty::detail::deref_if_pointer_like(this->index) - 1];
    } else {
        last_ticks = this->last_ticks_[9];
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
        this->last_kernel_ticks_[9] = std::move(stime);
        this->last_user_ticks_[9] = std::move(utime);
        this->last_ticks_[9] = std::move(ticks);
    }
    auto cpu_total = 0.0;
    if (rusty::detail::deref_if_pointer_like(this->index) < 10) {
        cpu_total = -1.0;
    } else {
        const auto busy = ((rusty::detail::deref_if_pointer_like(stime) - this->last_kernel_ticks_[8])) + ((rusty::detail::deref_if_pointer_like(utime) - this->last_user_ticks_[8]));
        cpu_total = ((static_cast<double>(busy))) / ((static_cast<double>((rusty::detail::deref_if_pointer_like(ticks) - this->last_ticks_[8]))));
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

} // export namespace rrr

// @safe - impl namespace: the cpuinfo_* sampling kernels. Each carries
// per-fn `// @unsafe` for syscalls (via the @safe rusty::sys::process
// helpers) and /proc parsing (std::ifstream + strtok/strtoul +
// operator>> chains). SP-5 (Cursor) remains the eventual refactor
// target for the two parsers.
namespace rrr {

// (anonymous namespace dissolved: the kernels need module linkage so the
// exported declarations above unify with these definitions)

// @unsafe - std::ifstream + getline + strtok with raw `char*` and
// strtoul on raw `char*` tokens.
void cpuinfo_get_network(CPUInfo& self, const std::string& pid,
                         rusty::Vec<double>* result_ptr, clock_t ticks) {
    rusty::Vec<double>& result = *result_ptr;
#ifndef __linux__
    (void) self;
    (void) pid;
    (void) ticks;
    result.push(-1.0);
    result.push(-1.0);
    return;
#else
    double tx_total = -1.0, rx_total = -1.0;
    std::string line;
    unsigned long txed = 0, rxed = 0;
    std::ifstream netfile("/proc/"+pid+"/net/dev");

    for(int i = 0; i < 4; i++){
        getline(netfile, line);
    }

    int i = 1;
    char* token = strtok(&line[0], " ");
    while(token != NULL){
        token = strtok(NULL, " ");
        if(i == 1){
            txed = strtoul(token, NULL, 0);
        }
        if(i == 9){
            rxed = strtoul(token, NULL, 0);
            break;
        }
        i++;
    }

    if(self.index < 10) {
        self.last_bytes_txed[self.index] = txed;
        self.last_bytes_rxed[self.index] = rxed;
    } else{
        for(int j = 0; j < 9; j++){
            self.last_bytes_txed[j] = self.last_bytes_txed[j+1];
            self.last_bytes_rxed[j] = self.last_bytes_rxed[j+1];
        }
        self.last_bytes_txed[9] = txed;
        self.last_bytes_rxed[9] = rxed;
    }

    if(ticks != self.last_ticks_[0]){
        if(self.index < 10){
            tx_total = -1.0;
            rx_total = -1.0;
        } else{
            tx_total = (txed-self.last_bytes_txed[8])/(ticks - self.last_ticks_[8]);
            rx_total = (rxed-self.last_bytes_rxed[8])/(ticks - self.last_ticks_[8]);
        }
    }

    result.push(tx_total);
    result.push(rx_total);

    self.last_txed = tx_total;
    self.last_rxed = rx_total;
#endif
}

// @unsafe - std::ifstream + a 24-step `operator>>` chain parsing
// /proc/{pid}/stat into a `long rss` field.
void cpuinfo_get_memory(CPUInfo& self, const std::string& pid,
                        rusty::Vec<double>* result_ptr, clock_t ticks) {
    rusty::Vec<double>& result = *result_ptr;
#ifndef __linux__
    (void) self;
    (void) pid;
    (void) ticks;
    result.push(-1.0);
    return;
#else
    long rss;
    double mem_usage, mem_total = -1.0;
    std::string ignore;

    std::ifstream stat_file("/proc/"+pid+"/stat", std::ios_base::in);

    stat_file >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore
              >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore
              >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> rss;

    mem_usage = rss * self.page_size;

    if(self.index < 10) {
        self.last_mem_usage[self.index] = mem_usage;
    } else{
        for(int j = 0; j < 9; j++){
            self.last_mem_usage[j] = self.last_mem_usage[j+1];
        }
        self.last_mem_usage[9] = mem_usage;
    }

    if(ticks != self.last_ticks_[0]){
        if(self.index < 10) mem_total = -1;
        else mem_total = (mem_usage - self.last_mem_usage[8])/(ticks - self.last_ticks_[8]);
    }

    result.push(mem_total);

    self.last_mem = mem_total;
#endif
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
    Log_debug("total amount of ram is: %lld", info.total_mem);

    info.page_size = rusty::sys::process::sysconf(_SC_PAGE_SIZE) / 1024;

    const auto ticks = rusty::sys::process::process_times();
    info.last_ticks_[info.index]        = static_cast<clock_t>(ticks.wall_ticks);
    info.last_kernel_ticks_[info.index] = static_cast<clock_t>(ticks.system_ticks);
    info.last_user_ticks_[info.index]   = static_cast<clock_t>(ticks.user_ticks);

    info.pid_ = rusty::sys::process::getpid();
    cpuinfo_get_network(info, std::to_string(info.pid_), &result, info.last_ticks_[info.index]);
    cpuinfo_get_memory(info, std::to_string(info.pid_), &result, info.last_ticks_[info.index]);

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
    Log_debug("ticks: %d -> %d", last_ticks, ticks);
}

// @safe - Rust-idiomatic singleton accessor.
//
// Equivalent in Rust:
//   static CPU_INFO: OnceLock<CpuInfo> = OnceLock::new();
//   pub fn cpu_stat() -> Vec<f64> {
//       CPU_INFO.get_or_init(CpuInfo::new).get_cpu_stat()
//   }
rusty::Vec<double> cpuinfo_cpu_stat() {
    static rusty::OnceCell<CPUInfo> inst;
    inst.get_or_init([]() -> CPUInfo { return cpuinfo_new(); });
    return inst.get_mut()->get_cpu_stat();
}

}  // namespace rrr
