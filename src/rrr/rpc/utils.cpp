module;

#include <rusty/result.hpp>
#include <rusty/sys/env.hpp>

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

// @safe - thin syscall wrappers. AddrInfo owns a raw `struct
// addrinfo*` from getaddrinfo / freeaddrinfo, so every accessor /
// mover that touches the pointer carries `// @unsafe`. The free
// functions set_nonblocking / find_open_port / get_host_name are all
// pure syscalls (fcntl, socket/bind/getsockname/close, gethostname)
// and are `// @unsafe`. Default ctor + `operator bool` (nullptr
// check) inherit namespace @safe.
export namespace rrr {

// @safe - see file header.
class AddrInfo {
private:
    struct addrinfo* info_{nullptr};

public:
    AddrInfo() = default;
    // @unsafe - takes raw `struct addrinfo*` ownership without checking.
    explicit AddrInfo(struct addrinfo* info) : info_(info) {}

    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;

    // @unsafe - raw pointer swap on the `info_` field.
    AddrInfo(AddrInfo&& other) noexcept : info_(other.info_) {
        other.info_ = nullptr;
    }

    // @unsafe - calls reset() (freeaddrinfo) and raw pointer swap.
    AddrInfo& operator=(AddrInfo&& other) noexcept {
        if (this != &other) {
            reset();
            info_ = other.info_;
            other.info_ = nullptr;
        }
        return *this;
    }

    // @unsafe - dtor runs freeaddrinfo via reset().
    ~AddrInfo() {
        reset();
    }

    // @unsafe - returns the raw `struct addrinfo*`.
    struct addrinfo* get() const { return info_; }
    // @unsafe - returns the raw `struct addrinfo*` for `->` access.
    struct addrinfo* operator->() const { return info_; }
    // @unsafe - dereferences the raw `struct addrinfo*`.
    struct addrinfo& operator*() const { return *info_; }
    explicit operator bool() const { return info_ != nullptr; }

    // @unsafe - swap-out + return raw `struct addrinfo*` (ownership transfer).
    struct addrinfo* release() {
        auto* p = info_;
        info_ = nullptr;
        return p;
    }

    // @unsafe - `freeaddrinfo` libc call on the raw `struct addrinfo*`.
    void reset(struct addrinfo* info = nullptr) {
        if (info_) {
            freeaddrinfo(info_);
        }
        info_ = info;
    }

    // @unsafe - `getaddrinfo` libc call returning raw `struct addrinfo*`.
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

// @safe - impl namespace. All three free functions are pure syscall
// wrappers and carry per-method `// @unsafe` below.
namespace rrr {

// @unsafe - fcntl(F_GETFL / F_SETFL) syscall.
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

// @unsafe - socket / bind / getsockname / close syscalls + C-style
// casts of `sockaddr_in*` to `sockaddr*` + raw `ai_addr` deref.
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

// @safe - rusty::sys::env::hostname returns an owned std::string and
// wraps gethostname in an inner @unsafe block. Returns "" on syscall
// failure (parity with the prior body).
std::string get_host_name() {
    std::string name = rusty::sys::env::hostname();
    if (name.empty()) {
        Log_error("Failed to get hostname.");
    }
    return name;
}

} // namespace rrr
