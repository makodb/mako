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
    unsigned long last_bytes_rxed[10] = {}, last_bytes_txed[10] = {}, last_mem_usage[10] = {};
    clock_t last_ticks_[10] = {}, last_user_ticks_[10] = {}, last_kernel_ticks_[10] = {};
    double last_cpu = 0, last_txed = 0, last_rxed = 0, last_mem = 0;
    long long total_mem = 0;
    long page_size = 0;
    int index = 0;
    pid_t pid_ = 0;

    // Mutex protecting the sample-history fields above. The original
    // code used `std::recursive_mutex`, but no callsite actually
    // recurses (the ctor and `get_cpu_stat()` are the only lock sites
    // and neither calls the other), so a plain non-recursive mutex is
    // enough. We use `rusty::Mutex<bool>` here as a movable lock
    // primitive — the payload `bool` is an unused placeholder; the
    // lock provides mutual exclusion for the surrounding fields the
    // Rust-idiomatic way would put those fields *inside* the
    // Mutex<T>, but restructuring CPUInfo to that shape is its own
    // refactor; the placeholder form gives us movability now.
    mutable rusty::Mutex<bool> mtx_{false};

    // @unsafe - mutex lock + dispatch into @unsafe get_network /
    // get_memory parsers. sysinfo / sysconf / times / getpid all flow
    // through @safe rusty::sys::process::* helpers.
    //
    // Replaces the previous user-declared private default ctor. The
    // class is now movable (`rusty::Mutex<bool>` has an explicit move
    // ctor that reinitializes the underlying std::mutex), so
    // `return info;` from a value-returning factory compiles cleanly.
    // No lock is taken here because `info` is local until returned —
    // no other thread can observe it during construction.
    static CPUInfo new_() {
        CPUInfo info;
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
        info.get_network(std::to_string(info.pid_), result, info.last_ticks_[info.index]);
        info.get_memory(std::to_string(info.pid_), result, info.last_ticks_[info.index]);

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

    // @unsafe - mutex lock + dispatch into the @unsafe get_network /
    // get_memory parsers. times() flows through
    // @safe rusty::sys::process::process_times.
    rusty::Vec<double> get_cpu_stat() {
        // Lock the placeholder mutex for mutual exclusion across
        // the field reads / writes below. Guard discarded immediately
        // — we hold the lock only for the scope of this function.
        auto _guard = mtx_.lock().unwrap();
        (void)_guard;

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
    //       CPU_INFO.get_or_init(CpuInfo::new).get_cpu_stat()
    //   }
    //
    // Lock-side note: `CPUInfo` is now movable (its `mtx_` field is a
    // `rusty::Mutex<bool>`, whose move ctor reinitializes the
    // underlying `std::mutex`), so `CPUInfo::new_()` can return by
    // value and the lambda below can pass it to `get_or_init` without
    // relying on mandatory-copy-elision tricks.
    static rusty::Vec<double> cpu_stat() {
        static rusty::OnceCell<CPUInfo> inst;
        inst.get_or_init([]() -> CPUInfo { return CPUInfo::new_(); });
        return inst.get_mut()->get_cpu_stat();
    }
};

} // export namespace rrr
