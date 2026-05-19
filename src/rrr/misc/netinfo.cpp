module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/times.h>

#include <rusty/sys/fs.hpp>

export module rrr.netinfo;

import std;

// @safe - NetInfo: gauges rx/tx bytes/second on ens4. The file reads
// go through `rusty::sys::fs::read_to_string` (no FILE*/ifstream
// escapes). Residual `times()` syscall and `strtoul` parses are
// wrapped in inline `// @unsafe { }` blocks.
export namespace rrr {

// @safe - see file header.
class NetInfo {
private:
    clock_t last_ticks_;
    unsigned long last_bytes_rxed, last_bytes_txed;

    static unsigned long parse_bytes(std::string_view path) {
        auto r = rusty::sys::fs::read_to_string(path);
        if (r.is_err()) return 0;
        std::string s = r.unwrap();
        unsigned long v = 0;
        // @unsafe { strtoul takes raw `const char*` + a `char**` endptr;
        //           matches the original silent-zero-on-junk semantics. }
        {
            v = strtoul(s.c_str(), NULL, 0);
        }
        return v;
    }

    NetInfo() {
        clock_t t = 0;
        // @unsafe { `times(&tms_buf)` syscall takes a raw `struct tms*`. }
        {
            struct tms tms_buf;
            t = times(&tms_buf);
        }
        last_ticks_ = t;
        last_bytes_rxed = parse_bytes("/sys/class/net/ens4/statistics/rx_bytes");
        last_bytes_txed = parse_bytes("/sys/class/net/ens4/statistics/tx_bytes");
    }

    double get_net_stat() {
        clock_t ticks = 0;
        // @unsafe { `times(&tms_buf)` syscall takes a raw `struct tms*`. }
        {
            struct tms tms_buf;
            ticks = times(&tms_buf);
        }
        if (ticks <= last_ticks_ + 1000000)
            return -1.0;

        unsigned long rxed = parse_bytes("/sys/class/net/ens4/statistics/rx_bytes");
        unsigned long txed = parse_bytes("/sys/class/net/ens4/statistics/tx_bytes");

        double ret = 0.0;
        ret += (txed - last_bytes_txed) + (rxed - last_bytes_rxed);
        ret /= (ticks - last_ticks_);

        last_ticks_ = ticks;
        last_bytes_rxed = rxed;
        last_bytes_txed = txed;

        return ret;
    }

public:
    static double net_stat() {
        static NetInfo net_info;
        return net_info.get_net_stat();
    }
};

} // export namespace rrr
