module;

#include <rusty/cell.hpp>
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

// @safe - thin syscall wrappers. AddrInfo owns a raw `struct addrinfo*`
// from getaddrinfo / freeaddrinfo. It is authored as an inline-rust DSL
// struct: the `#if RUSTYCPP_RUST` block is the source of truth and the
// transpiler regenerates the `RUSTYCPP:GEN-BEGIN ... END` C++ below it.
// `impl Drop` makes the freeaddrinfo-on-drop the destructor; the move
// machinery's `_rusty_forgotten` flag prevents a double free. The DSL
// derives copyability from fields, and a raw pointer is copyable, so a
// move-only `rusty::Cell<bool>` field (`owned_`) is what deletes the copy
// ctor — here load-bearing, since copying an owning addrinfo* would
// double free. Every raw-pointer body is an `@unsafe` free function
// below the struct. The free functions set_nonblocking / find_open_port
// / get_host_name are pure syscalls (fcntl, socket/bind/getsockname/
// close, gethostname) and are `// @unsafe`.
export namespace rrr {

// @unsafe - a null `struct addrinfo*` (default/invalid AddrInfo state).
inline struct addrinfo* addrinfo_null() { return nullptr; }
// @unsafe - nullptr check on the raw `struct addrinfo*`.
inline bool addrinfo_valid(struct addrinfo* info) { return info != nullptr; }
// @unsafe - `freeaddrinfo` libc call on the owned raw `struct addrinfo*`.
inline void addrinfo_free(struct addrinfo* info) {
    if (info) {
        freeaddrinfo(info);
    }
}

// @safe - owns a `struct addrinfo*`; see file header. Move-only via the
// `owned_` Cell marker; freed once on drop.
#if RUSTYCPP_RUST
struct AddrInfo {
    info_: *mut addrinfo,
    owned_: rusty::Cell<bool>,
}

impl AddrInfo {
    // Default: an invalid AddrInfo (null pointer).
    #[cpp_ctor]
    fn new() -> AddrInfo {
        AddrInfo { info_: addrinfo_null(), owned_: rusty::Cell::new(false) }
    }
    // Adopt ownership of a raw `struct addrinfo*` (e.g. from getaddrinfo).
    #[cpp_ctor]
    fn adopt(info: *mut addrinfo) -> AddrInfo {
        AddrInfo { info_: info, owned_: rusty::Cell::new(true) }
    }
    fn get(&self) -> *mut addrinfo {
        self.info_
    }
    fn valid(&self) -> bool {
        addrinfo_valid(self.info_)
    }
}

impl Drop for AddrInfo {
    fn drop(&mut self) {
        addrinfo_free(self.info_);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=utils.addrinfo version=1 rust_sha256=3fd23d765adad249edafb8386feea9ba7c1de045c106e34180e970312cfdcf77*/
struct AddrInfo;

struct AddrInfo {
    addrinfo* info_;
    rusty::Cell<bool> owned_;
    mutable bool _rusty_forgotten = false;
    AddrInfo(addrinfo* info__init, rusty::Cell<bool> owned__init) : info_(std::move(info__init)), owned_(std::move(owned__init)) {}
    AddrInfo(const AddrInfo&) = delete;
    AddrInfo(AddrInfo&& other) noexcept : info_(std::move(other.info_)), owned_(std::move(other.owned_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    AddrInfo& operator=(const AddrInfo&) = delete;
    AddrInfo& operator=(AddrInfo&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~AddrInfo();
        new (this) AddrInfo(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    AddrInfo();
    AddrInfo(addrinfo* info);
    addrinfo* get() const;
    bool valid() const;
    ~AddrInfo() noexcept(false);
};


AddrInfo::AddrInfo()
    : info_(addrinfo_null())
    , owned_(rusty::Cell<bool>::new_(false))
{}

AddrInfo::AddrInfo(addrinfo* info)
    : info_(std::move(info))
    , owned_(rusty::Cell<bool>::new_(true))
{}

addrinfo* AddrInfo::get() const {
    return this->info_;
}

bool AddrInfo::valid() const {
    return addrinfo_valid(this->info_);
}

AddrInfo::~AddrInfo() noexcept(false) {
    if (_rusty_forgotten) { return; }
    addrinfo_free(this->info_);
}
/*RUSTYCPP:GEN-END id=utils.addrinfo*/

// @unsafe - `getaddrinfo` libc call returning a raw `struct addrinfo*`,
// adopted into a move-only AddrInfo. (Was AddrInfo::resolve; lifted to a
// free function because the getaddrinfo out-parameter is not DSL-able.)
inline rusty::Result<AddrInfo, int> addrinfo_resolve(
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

    auto addr_result = addrinfo_resolve("0.0.0.0", nullptr, nullptr);
    if (addr_result.is_err()) {
        Log_error("Failed to getaddrinfo");
        ::close(fd);
        return -1;
    }
    auto local_addr = addr_result.unwrap();

    int port = -1;

    for (int i = 1024; i < 65000; ++i) {
        ((sockaddr_in*)local_addr.get()->ai_addr)->sin_port = i;
        if (::bind(fd, local_addr.get()->ai_addr, local_addr.get()->ai_addrlen) != 0) {
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
