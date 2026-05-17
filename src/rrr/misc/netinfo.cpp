module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/times.h>

export module rrr.netinfo;

import std;

export namespace rrr {

class NetInfo {
private:
    clock_t last_ticks_;
    unsigned long last_bytes_rxed, last_bytes_txed;
    NetInfo() {
        struct tms tms_buf;

        last_ticks_ = times(&tms_buf);

        std::string line1;
        std::ifstream rxfile("/sys/class/net/ens4/statistics/rx_bytes");
        getline(rxfile, line1);
        unsigned long rxed = strtoul(line1.c_str(), NULL, 0);
        rxfile.close();

        std::string line2;
        std::ifstream txfile("/sys/class/net/ens4/statistics/tx_bytes");
        getline(txfile, line2);
        unsigned long txed = strtoul(line2.c_str(), NULL, 0);
        txfile.close();

        last_bytes_rxed = rxed;
        last_bytes_txed = txed;
    }

    double get_net_stat() {
        struct tms tms_buf;
        clock_t ticks;
        double ret = 0.0;

        ticks = times(&tms_buf);
        if (ticks <= last_ticks_ + 1000000)
            return -1.0;

        std::string line1;
        std::ifstream rxfile("/sys/class/net/ens4/statistics/rx_bytes");
        getline(rxfile, line1);
        unsigned long rxed = strtoul(line1.c_str(), NULL, 0);
        rxfile.close();

        std::string line2;
        std::ifstream txfile("/sys/class/net/ens4/statistics/tx_bytes");
        getline(txfile, line2);
        unsigned long txed = strtoul(line2.c_str(), NULL, 0);
        txfile.close();

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
