module;

#include <rusty/cell.hpp>
#include <rusty/result.hpp>
// Reachability: this partition's GEN names
// `rusty::detail::mark_forgotten_if_supported` (the recursive drop-glue
// suppression emitted into `rusty_mark_forgotten`), and that helper is
// header-only — it is not in the transpiled `rusty` module. A GMF must
// include what its own GEN names.
// Reachability again: the GEN now names `rusty::ptr::null_mut()` for the
// DSL's `core::ptr::null_mut()`.
#include <rusty/ptr.hpp>
// Reachability: get_host_name's GEN names `rusty::is_empty`.
#include <rusty/array.hpp>
#include <rusty/slice.hpp>
// Reachability: the rrr-internal log sites now call log_line directly, and
// its GEN names rusty::clone on the level constant.
#include <rusty/rusty.hpp>
#include <rusty/sys/env.hpp>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
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
// below the struct. `set_nonblocking` is the one hand-written syscall
// wrapper left (fcntl) and is `// @unsafe`; `find_open_port` and
// `get_host_name` are DSL — the first a shim over the plain-C
// `srpc_find_open_port` ladder, the second over rusty::sys::env::hostname.
export namespace rrr {

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
        AddrInfo { info_: core::ptr::null_mut(), owned_: rusty::Cell::new(false) }
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
        !self.info_.is_null()
    }
}

impl Drop for AddrInfo {
    fn drop(&mut self) {
        if !self.info_.is_null() {
            unsafe { freeaddrinfo(self.info_); }
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=utils.addrinfo version=1 rust_sha256=1d64fe0ea13a5236550f4ce60d2ffb6162f1777a1465bf19b6d5950c29e13fc7*/
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
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->info_); rusty::detail::mark_forgotten_if_supported(this->owned_); }


    AddrInfo();
    AddrInfo(addrinfo* info);
    addrinfo* get() const;
    bool valid() const;
    ~AddrInfo() noexcept(false);
};


AddrInfo::AddrInfo()
    : info_(rusty::ptr::null_mut())
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
    return rusty::detail::rust_not((this->info_ == nullptr));
}

AddrInfo::~AddrInfo() noexcept(false) {
    if (_rusty_forgotten) { return; }
    if (rusty::detail::rust_not((this->info_ == nullptr))) {
        // @unsafe
        {
            freeaddrinfo(this->info_);
        }
    }
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

// The syscall ladder (socket/getaddrinfo/bind/getsockname/close) lives
// in srpc_net.c now (plain C, Goal-0 C demotion). Contract: port on
// success, 0 if nothing free in 1024..64999, -1 on syscall failure.
extern "C" int srpc_find_open_port(void);

// @unsafe - thin shim over the C kernel; logging stays C++-side.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Unlike get_host_name below, the Log_* calls need NO `rrr::`
// qualification: the return-type free-fn misresolution fires only for a fn
// returning a CLASS type, and this one returns i32 (playbook 7.12,
// re-probed at pin da6e9bf4).
//
// Explicit block id: auto-numbering names a block by POSITION, so a block
// here would be emitted as `utils.2` and collide with the get_host_name
// block below.
#if RUSTYCPP_RUST
fn find_open_port() -> i32 {
    let port: i32 = srpc_find_open_port();
    if port > 0 {
        log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Found open port: {}", port));
        return port;
    }
    log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Failed to find open port."));
    -1
}
#endif
/*RUSTYCPP:GEN-BEGIN id=utils.find_open_port version=1 rust_sha256=0ef5721588cc4e59b0ab5238d3ab1bc0a4702b6802096578cdd78dd1aaf87b63*/
int32_t find_open_port();

int32_t find_open_port() {
    int32_t port = srpc_find_open_port();
    if (rusty::detail::deref_if_pointer_like(port) > 0) {
        log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Found open port: {}", std::move(port)));
        return std::move(port);
    }
    log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Failed to find open port."));
    return -1;
}
/*RUSTYCPP:GEN-END id=utils.find_open_port*/

// @safe - rusty::sys::env::hostname returns an owned std::string and
// wraps gethostname in an inner @unsafe block. Returns "" on syscall
// failure (parity with the prior body).
#if RUSTYCPP_RUST
fn get_host_name() -> std::string {
    let name: std::string = rusty::sys::env::hostname();
    let missing: bool = name.is_empty();
    if missing {
        // rrr:: qualification dodges a transpiler defect: a fn returning
        // std::string mis-resolves UNQUALIFIED free-fn calls against the
        // return type (emitted std::string::Log_error). Upstream fix queued.
        rrr::log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Failed to get hostname."));
    }
    name
}
#endif
/*RUSTYCPP:GEN-BEGIN id=utils.2 version=1 rust_sha256=51c931b9171ea5f69825daf9a8f7d8be7648a054e187b8d02515a2738ad15aca*/
std::string get_host_name();

std::string get_host_name() {
    std::string name = rusty::sys::env::hostname();
    const bool missing = rusty::is_empty(name);
    if (missing) {
        rrr::log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Failed to get hostname."));
    }
    return std::move(name);
}
/*RUSTYCPP:GEN-END id=utils.2*/

} // namespace rrr
