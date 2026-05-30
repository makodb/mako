module;

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
import rrr.logging;

// @safe - CPUInfo: process-level cpu/network/memory sampling. The
// ctor, `get_cpu_stat`, `get_network`, and `get_memory` all carry
// per-method `// @unsafe` because they do syscalls (sysinfo, sysconf,
// times, getpid) and parse /proc files via std::ifstream +
// operator>> / strtok / strtoul. The `cpu_stat()` factory just hands
// out the static instance and inherits namespace @safe.
export namespace rrr {

// @safe - see file header.
class CPUInfo {
private:
    unsigned long last_bytes_rxed[10], last_bytes_txed[10], last_mem_usage[10];
    clock_t last_ticks_[10], last_user_ticks_[10], last_kernel_ticks_[10];
    double last_cpu, last_txed, last_rxed, last_mem;
    long long total_mem;
    long page_size;
    int index = 0;
    pid_t pid_;
    std::recursive_mutex mtx_;
    // @unsafe - std::recursive_mutex lock + dispatch into @unsafe
    // get_network / get_memory parsers. sysinfo / sysconf / times /
    // getpid all flow through @safe rusty::sys::process::* helpers.
    //
    // Kept as a private user-declared constructor (not a `static new_()`
    // factory) because `std::recursive_mutex` is neither copyable nor
    // movable, which makes `CPUInfo` non-movable as a whole. A
    // value-returning `new_()` would need NRVO-or-move-ctor access for
    // its `return info;`, but the implicit move is deleted; the C++
    // language requires an accessible move/copy ctor for the return
    // even when NRVO would elide it. The `OnceCell<CPUInfo>` access
    // path constructs `CPUInfo{}` as a prvalue (mandatory copy
    // elision), which bypasses that requirement, so the ctor stays.
    CPUInfo() {
        const std::lock_guard<std::recursive_mutex> lock (mtx_);
#ifdef __linux__
        rusty::Vec<double> result;

        const auto mem_info = rusty::sys::process::sysinfo();
        // `mem_info.total_ram_bytes` is already scaled by mem_unit.
        total_mem = static_cast<long long>(mem_info.total_ram_bytes / 1024);
        Log_debug("total amount of ram is: %lld", total_mem);

        page_size = rusty::sys::process::sysconf(_SC_PAGE_SIZE) / 1024;

        const auto ticks = rusty::sys::process::process_times();
        last_ticks_[index]        = static_cast<clock_t>(ticks.wall_ticks);
        last_kernel_ticks_[index] = static_cast<clock_t>(ticks.system_ticks);
        last_user_ticks_[index]   = static_cast<clock_t>(ticks.user_ticks);

        pid_ = rusty::sys::process::getpid();
        get_network(std::to_string(pid_), result, last_ticks_[index]);
        get_memory(std::to_string(pid_), result, last_ticks_[index]);

        index++;
#else
        last_cpu = last_txed = last_rxed = last_mem = 0.0;
        total_mem = 0;
        page_size = 0;
        index = 0;
        pid_ = rusty::sys::process::getpid();
#endif
    }

    // @unsafe - std::recursive_mutex lock + dispatch into the @unsafe
    // get_network / get_memory parsers. times() flows through
    // @safe rusty::sys::process::process_times.
    rusty::Vec<double> get_cpu_stat() {
        const std::lock_guard<std::recursive_mutex> lock (mtx_);

        rusty::Vec<double> result;
        double cpu_total;
        clock_t last_ticks;

        const auto sample = rusty::sys::process::process_times();
        const clock_t ticks  = static_cast<clock_t>(sample.wall_ticks);
        const clock_t stime  = static_cast<clock_t>(sample.system_ticks);
        const clock_t utime  = static_cast<clock_t>(sample.user_ticks);
        if(index < 10) last_ticks = last_ticks_[index-1];
        else last_ticks = last_ticks_[9];

        Log_debug("ticks: %d -> %d", last_ticks, ticks);
        if (ticks <= last_ticks + 60){
            if(index < 10){
                return {-1.0, -1.0, -1.0, -1.0};
            } else{
                return {last_cpu, last_txed, last_rxed, last_mem};
            }
        }

        if(index < 10){
            last_kernel_ticks_[index] = stime;
            last_user_ticks_[index] = utime;
            last_ticks_[index] = ticks;
            index++;
        } else{
            for(int i = 0; i < 9; i++){
                last_kernel_ticks_[i] = last_kernel_ticks_[i+1];
                last_user_ticks_[i] = last_user_ticks_[i+1];
                last_ticks_[i] = last_ticks_[i+1];
            }
            last_kernel_ticks_[9] = stime;
            last_user_ticks_[9] = utime;
            last_ticks_[9] = ticks;
        }

        if(index < 10){
            cpu_total = -1.0;
        } else{
            cpu_total = (stime - last_kernel_ticks_[8]) +
                (utime - last_user_ticks_[8]);
            cpu_total /= (ticks - last_ticks_[8]);
        }
        last_cpu = cpu_total;

        if(index < 10) result.push(-1.0);
        else result.push(cpu_total);

        get_network(std::to_string(pid_), result, ticks);
        get_memory(std::to_string(pid_), result, ticks);
        return result;
    }

    // @unsafe - std::ifstream + getline + strtok with raw `char*` and
    // strtoul on raw `char*` tokens. SP-5 (Cursor) is the eventual
    // refactor target.
    void get_network(const std::string& pid, rusty::Vec<double>& result, clock_t ticks){
#ifndef __linux__
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

        if(index < 10) {
            last_bytes_txed[index] = txed;
            last_bytes_rxed[index] = rxed;
        } else{
            for(int i = 0; i < 9; i++){
                last_bytes_txed[i] = last_bytes_txed[i+1];
                last_bytes_rxed[i] = last_bytes_rxed[i+1];
            }
            last_bytes_txed[9] = txed;
            last_bytes_rxed[9] = rxed;
        }

        if(ticks != last_ticks_[0]){
            if(index < 10){
                tx_total = -1.0;
                rx_total = -1.0;
            } else{
                tx_total = (txed-last_bytes_txed[8])/(ticks - last_ticks_[8]);
                rx_total = (rxed-last_bytes_rxed[8])/(ticks - last_ticks_[8]);
            }
        }

        result.push(tx_total);
        result.push(rx_total);

        last_txed = tx_total;
        last_rxed = rx_total;
#endif
    }

    // @unsafe - std::ifstream + a 24-step `operator>>` chain parsing
    // /proc/{pid}/stat into a `long rss` field. SP-5 (Cursor) is the
    // eventual refactor target.
    void get_memory(const std::string& pid, rusty::Vec<double>& result, clock_t ticks){
#ifndef __linux__
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

        mem_usage = rss * page_size;

        if(index < 10) {
            last_mem_usage[index] = mem_usage;
        } else{
            for(int i = 0; i < 9; i++){
                last_mem_usage[i] = last_mem_usage[i+1];
            }
            last_mem_usage[9] = mem_usage;
        }

        if(ticks != last_ticks_[0]){
            if(index < 10) mem_total = -1;
            else mem_total = (mem_usage - last_mem_usage[8])/(ticks - last_ticks_[8]);
        }

        result.push(mem_total);

        last_mem = mem_total;
#endif
    }

public:
    // @safe - Rust-idiomatic singleton accessor.
    //
    // Equivalent in Rust:
    //   static CPU_INFO: OnceLock<CpuInfo> = OnceLock::new();
    //   pub fn cpu_stat() -> Vec<f64> {
    //       CPU_INFO.get_or_init(|| CpuInfo::new()).get_cpu_stat()
    //   }
    //
    // The previous form used a C++11 function-local static (Meyers
    // singleton), which is semantically the same but doesn't have a
    // direct Rust-DSL counterpart. With `rusty::OnceCell<T>` we get a
    // one-to-one mapping to `std::sync::OnceLock<T>`. `get_or_init`
    // direct-initializes the cell from the lambda's prvalue (C++17
    // mandatory copy elision), so CPUInfo's `std::recursive_mutex`
    // field — which is non-movable — does not block this.
    //
    // `get_cpu_stat()` mutates internal sample history, so we reach
    // it via `inst.get_mut()` (guaranteed non-null after init).
    static rusty::Vec<double> cpu_stat() {
        static rusty::OnceCell<CPUInfo> inst;
        inst.get_or_init([]() -> CPUInfo { return CPUInfo{}; });
        return inst.get_mut()->get_cpu_stat();
    }
};

} // export namespace rrr
