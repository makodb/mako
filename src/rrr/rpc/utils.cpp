module;

#include <sys/types.h>
#include <stdarg.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>
#include <rusty/result.hpp>


#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>






module rrr:impl.rpc.utils;

import <list>;
import <map>;
import <unordered_map>;
import <functional>;
import <utility>;
import rrr;

// Note: std::atomic public API (load, store, etc.) is annotated in event.h
// No external annotations needed here - utils.cpp doesn't use atomics directly


using namespace std;

namespace rrr {

// @unsafe - Calls fcntl (external unsafe)
// SAFETY: fcntl is POSIX-compliant, fd must be valid
int set_nonblocking(int fd, bool nonblocking) {
    int ret = fcntl(fd, F_GETFL, 0);
    if (ret != -1) {
        if (nonblocking) {
            ret = fcntl(fd, F_SETFL, ret | O_NONBLOCK);
        } else {
            ret = fcntl(fd, F_SETFL, ret & ~O_NONBLOCK);
        }
    }
    return ret;
}

// @unsafe - Uses address-of operations for socket functions
// SAFETY: All pointers remain valid throughout function scope
int find_open_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        Log_error("Failed to create socket");
        return -1;
    }

    // Use AddrInfo RAII wrapper - automatically frees on scope exit
    auto addr_result = AddrInfo::resolve("0.0.0.0", nullptr, nullptr);
    if (addr_result.is_err()) {
        Log_error("Failed to getaddrinfo");
        ::close(fd);
        return -1;
    }
    auto local_addr = addr_result.unwrap();

    int port = -1;

    for (int i = 1024; i < 65000; ++i) {
        ((sockaddr_in*)local_addr->ai_addr)->sin_port = i;
        if (::bind(fd, local_addr->ai_addr, local_addr->ai_addrlen) != 0) {
            continue;
        }

        sockaddr_in addr;
        socklen_t addrlen;
        memset(&addr, 0, sizeof(addr));
        if (getsockname(fd, (sockaddr*)&addr, &addrlen) != 0) {
            Log_error("Failed to get socket address");
            ::close(fd);
            return -1;  // AddrInfo automatically freed
        }

        port = i;
        break;
    }

    // AddrInfo automatically freed when local_addr goes out of scope
    ::close(fd);

    if (port != -1) {
        Log_info("Found open port: %d", port);
        return port;
    }

    Log_error("Failed to find open port.");
    return -1;
}

// @unsafe - Uses raw buffer for gethostname system call
std::string get_host_name() {
    char buffer[1024];
    if (gethostname(buffer, 1024) != 0) {
        Log_error("Failed to get hostname.");
        return "";
    }
    return std::string(buffer);
}

} // namespace rrr
