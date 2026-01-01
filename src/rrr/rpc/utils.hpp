#pragma once

#include <list>
#include <map>
#include <unordered_map>
#include <functional>

#include <sys/types.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <inttypes.h>
#include <netdb.h>

#include <rusty/result.hpp>

// External annotations for system functions that cannot have in-place annotations
// Note: Log functions now have in-place annotations in logging.hpp
// @external: {
//   fcntl: [unsafe, (int, int, ...) -> int]
//   socket: [unsafe, (int, int, int) -> int]
//   bind: [unsafe, (int, const sockaddr*, socklen_t) -> int]
//   getsockname: [unsafe, (int, sockaddr*, socklen_t*) -> int]
//   gethostname: [unsafe, (char*, size_t) -> int]
//   bzero: [unsafe, (void*, size_t) -> void]
//   memset: [unsafe, (void*, int, size_t) -> void*]
//   getaddrinfo: [unsafe, (const char*, const char*, const addrinfo*, addrinfo**) -> int]
//   freeaddrinfo: [unsafe, (addrinfo*) -> void]
//   close: [unsafe, (int) -> int]
// }

namespace rrr {

// RAII wrapper for addrinfo from getaddrinfo()
// Automatically calls freeaddrinfo() on destruction
// @safe - RAII destructor handles cleanup
class AddrInfo {
private:
    struct addrinfo* info_{nullptr};

public:
    AddrInfo() = default;
    explicit AddrInfo(struct addrinfo* info) : info_(info) {}

    // No copy - ownership is unique
    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;

    // Move OK
    AddrInfo(AddrInfo&& other) noexcept : info_(other.info_) {
        other.info_ = nullptr;
    }

    // @lifetime: (&'a mut) -> &'a mut
    AddrInfo& operator=(AddrInfo&& other) noexcept {
        if (this != &other) {
            reset();
            info_ = other.info_;
            other.info_ = nullptr;
        }
        return *this;
    }

    // Destructor calls freeaddrinfo automatically
    ~AddrInfo() {
        reset();
    }

    // Access the underlying pointer
    struct addrinfo* get() const { return info_; }
    struct addrinfo* operator->() const { return info_; }
    // @lifetime: (&'a) -> &'a
    struct addrinfo& operator*() const { return *info_; }
    explicit operator bool() const { return info_ != nullptr; }

    // Release ownership (for storing in member variable)
    struct addrinfo* release() {
        auto* p = info_;
        info_ = nullptr;
        return p;
    }

    // Reset and free current, optionally take new pointer
    void reset(struct addrinfo* info = nullptr) {
        if (info_) {
            // @unsafe
            {
                freeaddrinfo(info_);
            }
        }
        info_ = info;
    }

    // Factory: wraps getaddrinfo with Result
    // @unsafe - Calls getaddrinfo system function
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

// @unsafe - Calls fcntl (external unsafe)
int set_nonblocking(int fd, bool nonblocking);

// @unsafe - Uses address-of operations for socket functions
int find_open_port();

// @unsafe - Uses raw buffer for gethostname system call
std::string get_host_name();

} // namespace rrr
