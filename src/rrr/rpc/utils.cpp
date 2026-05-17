module;

#include <rusty/result.hpp>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

export module rrr.utils;

import std;
import rrr.logging;

export namespace rrr {

class AddrInfo {
private:
    struct addrinfo* info_{nullptr};

public:
    AddrInfo() = default;
    explicit AddrInfo(struct addrinfo* info) : info_(info) {}

    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;

    AddrInfo(AddrInfo&& other) noexcept : info_(other.info_) {
        other.info_ = nullptr;
    }

    AddrInfo& operator=(AddrInfo&& other) noexcept {
        if (this != &other) {
            reset();
            info_ = other.info_;
            other.info_ = nullptr;
        }
        return *this;
    }

    ~AddrInfo() {
        reset();
    }

    struct addrinfo* get() const { return info_; }
    struct addrinfo* operator->() const { return info_; }
    struct addrinfo& operator*() const { return *info_; }
    explicit operator bool() const { return info_ != nullptr; }

    struct addrinfo* release() {
        auto* p = info_;
        info_ = nullptr;
        return p;
    }

    void reset(struct addrinfo* info = nullptr) {
        if (info_) {
            freeaddrinfo(info_);
        }
        info_ = info;
    }

    static rusty::Result<AddrInfo, int> resolve(
        const char* host,
        const char* service,
        const struct addrinfo* hints
    ) {
        struct addrinfo* result = nullptr;
        int r = getaddrinfo(host, service, hints, &result);
        if (r != 0) {
            return rusty::Err<AddrInfo, int>(r);
        }
        return rusty::Ok<AddrInfo, int>(AddrInfo(result));
    }
};

int set_nonblocking(int fd, bool nonblocking);
int find_open_port();
std::string get_host_name();

} // export namespace rrr

namespace rrr {

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

int find_open_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        Log_error("Failed to create socket");
        return -1;
    }

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
            return -1;
        }

        port = i;
        break;
    }

    ::close(fd);

    if (port != -1) {
        Log_info("Found open port: %d", port);
        return port;
    }

    Log_error("Failed to find open port.");
    return -1;
}

std::string get_host_name() {
    char buffer[1024];
    if (gethostname(buffer, 1024) != 0) {
        Log_error("Failed to get hostname.");
        return "";
    }
    return std::string(buffer);
}

} // namespace rrr
