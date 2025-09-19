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

// External annotations for system functions used in utils
// @external: {
//   fcntl: [safe, (int, int, ...) -> int]
//   socket: [safe, (int, int, int) -> int]
//   bind: [safe, (int, const sockaddr*, socklen_t) -> int]
//   getsockname: [safe, (int, sockaddr*, socklen_t*) -> int]
//   gethostname: [safe, (char*, size_t) -> int]
//   bzero: [safe, (void*, size_t) -> void]
//   memset: [safe, (void*, int, size_t) -> void*]
//   getaddrinfo: [safe, (const char*, const char*, const addrinfo*, addrinfo**) -> int]
//   freeaddrinfo: [safe, (addrinfo*) -> void]
//   close: [safe, (int) -> int]
//   rrr::Log::info: [safe, (int, const char*, const char*, ...) -> void]
//   rrr::Log::error: [safe, (int, const char*, const char*, ...) -> void]
//   rrr::Log::debug: [safe, (int, const char*, const char*, ...) -> void]
//   rrr::Log::warn: [safe, (int, const char*, const char*, ...) -> void]
//   rrr::Log::fatal: [safe, (int, const char*, const char*, ...) -> void]
// }

namespace rrr {

// @safe - Uses safe fcntl operations  
int set_nonblocking(int fd, bool nonblocking);

// @unsafe - Uses address-of operations for socket functions
int find_open_port();

// @unsafe - Calls unsafe Log::error on failure
std::string get_host_name();

} // namespace rrr
